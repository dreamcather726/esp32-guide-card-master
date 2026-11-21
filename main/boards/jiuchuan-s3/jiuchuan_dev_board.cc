#include "rndis_board.h"
#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "led/circular_strip.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "assets/lang_config.h"
#include "location_mqtt_client.h"
#include "settings.h"
#include "startup_qr_jpeg.h"
#include "startup_flow.h"
#include "navigation_overlay.h"
#include "guide_pages.h"
#include <ssid_manager.h>

#include <driver/i2c_master.h>
#include <driver/sdmmc_host.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <http.h>
#include <sdmmc_cmd.h>

#if defined(CONFIG_BT_NIMBLE_ROLE_OBSERVER) && !defined(CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING)
#include <host/ble_hs.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <mutex>
#include <memory>
#include <new>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>

#include "power_save_timer.h"
#include "power_manager.h"
#include "gpio_manager.h"

#define BOARD_TAG "JiuchuanDevBoard"

namespace {
constexpr uint32_t kModemRetryMarker = 0x4d4c3337;
RTC_NOINIT_ATTR uint32_t modem_retry_marker;

bool UseEarlyUsbPowerOnRetry() {
    if (esp_reset_reason() != ESP_RST_SW) modem_retry_marker = 0;
    return modem_retry_marker == kModemRetryMarker;
}

#if defined(CONFIG_BT_NIMBLE_ROLE_OBSERVER) && !defined(CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING)
constexpr const char* kBeaconTag = "BleBeacon";
constexpr const char* kBeaconMacFilter = "D9:62:62";

int OnBleBeaconReport(ble_gap_event* event, void*) {
    if (event->type != BLE_GAP_EVENT_DISC) return 0;
    const auto& report = event->disc;
    const auto* address = report.addr.val;
    char mac[18];
    std::snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                  static_cast<unsigned>(address[5]), static_cast<unsigned>(address[4]),
                  static_cast<unsigned>(address[3]), static_cast<unsigned>(address[2]),
                  static_cast<unsigned>(address[1]), static_cast<unsigned>(address[0]));
    if (std::strstr(mac, kBeaconMacFilter) == nullptr) return 0;
    if (report.rssi == 127) {
        ESP_LOGI(kBeaconTag, "MAC=%s RSSI=unknown", mac);
    } else {
        LocationMqttClient::RecordBleBeacon(mac, report.rssi);
        char diagnostic[40];
        std::snprintf(diagnostic, sizeof(diagnostic), "BLE %s %d", mac, report.rssi);
        Board::GetInstance().GetDisplay()->SetDiagnosticStatus(DiagnosticChannel::Ble, diagnostic);
        ESP_LOGI(kBeaconTag, "MAC=%s RSSI=%d dBm", mac, report.rssi);
    }
    return 0;
}

void OnBleBeaconSync() {
    uint8_t own_address_type = 0;
    int result = ble_hs_util_ensure_addr(0);
    if (result == 0) result = ble_hs_id_infer_auto(0, &own_address_type);
    if (result == 0) {
        ble_gap_disc_params scan = {};
        scan.passive = 1;
        scan.filter_duplicates = 1;
        scan.itvl = 160;
        scan.window = 48;
        result = ble_gap_disc(own_address_type, BLE_HS_FOREVER, &scan, OnBleBeaconReport, nullptr);
    }
    if (result == 0) {
        Board::GetInstance().GetDisplay()->SetDiagnosticStatus(DiagnosticChannel::Ble, "BLE SCAN D9:62:62");
        ESP_LOGI(kBeaconTag, "Passive BLE scan started; MAC contains %s", kBeaconMacFilter);
    } else if (result != BLE_HS_EALREADY) {
        char diagnostic[40];
        std::snprintf(diagnostic, sizeof(diagnostic), "BLE SCAN E%d", result);
        Board::GetInstance().GetDisplay()->SetDiagnostic(DiagnosticChannel::Ble, diagnostic);
        ESP_LOGE(kBeaconTag, "BLE scan failed: %d", result);
    }
}

void StartBleBeaconScan() {
    static bool started = false;
    if (started) return;
    const esp_err_t result = nimble_port_init();
    if (result != ESP_OK) {
        char diagnostic[40];
        std::snprintf(diagnostic, sizeof(diagnostic), "BLE INIT E%X", static_cast<unsigned>(result));
        Board::GetInstance().GetDisplay()->SetDiagnostic(DiagnosticChannel::Ble, diagnostic);
        ESP_LOGE(kBeaconTag, "BLE init failed: %s", esp_err_to_name(result));
        return;
    }
    ble_hs_cfg.sync_cb = OnBleBeaconSync;
    if (xTaskCreatePinnedToCore([](void*) {
            nimble_port_run();
            vTaskDelete(nullptr);
        }, "nimble_host", CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE, nullptr, configMAX_PRIORITIES - 4,
        nullptr, CONFIG_BT_NIMBLE_PINNED_TO_CORE) != pdPASS) {
        Board::GetInstance().GetDisplay()->SetDiagnostic(DiagnosticChannel::Ble, "BLE TASK NO MEM");
        ESP_LOGE(kBeaconTag, "BLE host task creation failed; internal_largest=%u",
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        const auto cleanup = nimble_port_deinit();
        if (cleanup != ESP_OK) ESP_LOGE(kBeaconTag, "BLE cleanup failed: %s", esp_err_to_name(cleanup));
        return;
    }
    started = true;
}
#endif

bool IsCellularNetworkConfigured() {
    Settings network_settings("network", true);
    if (network_settings.GetInt("type", 1) != 0) {
        return true;
    }

    Settings device_settings("device", false);
    const bool has_cached_imei = !device_settings.GetString("imei").empty();
    const bool has_saved_wifi =
        !SsidManager::GetInstance().GetSsidList().empty();
    if (has_cached_imei && has_saved_wifi) {
        return false;
    }

    ESP_LOGW(BOARD_TAG,
             "WiFi mode prerequisites missing (imei=%d wifi=%d); falling back to GSM",
             has_cached_imei, has_saved_wifi);
    network_settings.SetInt("type", 1);
    return true;
}

bool PrepareBoardForStartup() {
    gpio_deep_sleep_hold_dis();
    if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
        gpio_hold_dis(DISPLAY_BACKLIGHT_PIN);

        // The LCD backlight is driven through the S8550 high-side transistor
        // shown in the schematic. Keep it on during panel bring-up so a
        // working LCD is not mistaken for a dead display while LEDC starts.
        gpio_config_t backlight_config = {};
        backlight_config.intr_type = GPIO_INTR_DISABLE;
        backlight_config.mode = GPIO_MODE_OUTPUT;
        backlight_config.pin_bit_mask = 1ULL << DISPLAY_BACKLIGHT_PIN;
        backlight_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        backlight_config.pull_up_en = GPIO_PULLUP_DISABLE;
        const esp_err_t backlight_result = gpio_config(&backlight_config);
        if (backlight_result == ESP_OK) {
            gpio_set_level(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_ON_LEVEL);
            ESP_LOGI(BOARD_TAG,
                     "Early LCD backlight drive: GPIO%d=%d (on_level=%d)",
                     DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_ON_LEVEL,
                     DISPLAY_BACKLIGHT_ON_LEVEL);
        } else {
            ESP_LOGE(BOARD_TAG, "Early LCD backlight GPIO config failed: %s",
                     esp_err_to_name(backlight_result));
        }
    }
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
        rtc_gpio_deinit(SHUTDOWN_WAKE_GPIO);
        ESP_LOGI(BOARD_TAG, "Woke from remote shutdown via BOOT (GPIO%d)",
                 SHUTDOWN_WAKE_GPIO);
    }
    return IsCellularNetworkConfigured();
}
}  // namespace

extern "C" const lv_image_dsc_t jiuchuan_logo;

class NoEmojiSpiLcdDisplay : public SpiLcdDisplay {
private:
    static constexpr int kSubtitlePanelWidth = 220;
    static constexpr int kSubtitleTextWidth = 204;
    static constexpr int kSubtitleVisibleLines = 3;
    static constexpr int kSubtitlePanelPadding = 8;
    static constexpr int kStartupLabelOffsetY = 25;
    static constexpr uint32_t kSubtitleScrollIntervalMs = 1800;
    static constexpr uint32_t kSubtitleScrollAnimationMs = 300;

    lv_obj_t* subtitle_viewport_ = nullptr;
    lv_obj_t* content_footer_label_ = nullptr;
    lv_timer_t* subtitle_scroll_timer_ = nullptr;
    std::unique_ptr<NavigationOverlay> navigation_;
    std::unique_ptr<GuidePages> guide_pages_;
    GuideView guide_view_;
    NavigationStatus navigation_status_;
    std::mutex diagnostic_mutex_;
    struct DiagnosticLine {
        std::array<char, 40> text{};
        bool fault = false;
    };
    std::array<DiagnosticLine, static_cast<size_t>(DiagnosticChannel::Count)> diagnostics_{};
    std::atomic<bool> diagnostic_page_open_{false};
    bool diagnostic_dirty_ = false;
    lv_obj_t* diagnostic_label_ = nullptr;
    const RndisBoard& rndis_board_;
    int subtitle_scroll_offset_ = 0;
    int subtitle_scroll_max_offset_ = 0;
    int subtitle_line_height_ = 25;
    int subtitle_viewport_height_ = 75;
    bool chat_message_visible_ = false;
    bool startup_label_visible_ = false;
    std::unique_ptr<LvglImage> startup_qr_image_;
    lv_obj_t* startup_overlay_ = nullptr;
    lv_obj_t* startup_qr_widget_ = nullptr;
    lv_obj_t* startup_imei_label_ = nullptr;
    lv_timer_t* startup_timer_ = nullptr;
    bool startup_finished_ = false;
    std::function<void()> startup_completion_;

    void InitDiagnostics() {
        diagnostic_label_ = lv_label_create(lv_screen_active());
        lv_obj_set_width(diagnostic_label_, width_ - 12);
        lv_label_set_long_mode(diagnostic_label_, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(diagnostic_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(diagnostic_label_, lv_color_white(), 0);
        lv_obj_set_style_text_line_space(diagnostic_label_, -1, 0);
        lv_obj_set_style_bg_color(diagnostic_label_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(diagnostic_label_, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(diagnostic_label_, 3, 0);
        lv_obj_align(diagnostic_label_, LV_ALIGN_BOTTOM_MID, 0, -21);
        lv_obj_add_flag(diagnostic_label_, LV_OBJ_FLAG_HIDDEN);
    }

    void RefreshDiagnostics(bool force = false) {
        decltype(diagnostics_) snapshot;
        {
            std::lock_guard lock(diagnostic_mutex_);
            if (!diagnostic_dirty_ && !force) return;
            snapshot = diagnostics_;
            diagnostic_dirty_ = false;
        }
        std::string text;
        const bool show_all = diagnostic_page_open_.load();
        const std::array<const char*, static_cast<size_t>(DiagnosticChannel::Count)> defaults = {
            "USB WAIT", "NET WAIT", "QR WAIT", "WAV WAIT",
            "BLE WAIT", "BIZ MQTT WAIT", "POS MQTT WAIT", "KJ WAIT",
        };
        for (size_t i = 0; i < snapshot.size(); ++i) {
            const auto& line = snapshot[i];
            const bool empty = line.text.front() == '\0';
            if (!show_all && (!line.fault || empty)) continue;
            if (!text.empty()) text += '\n';
            text += empty ? defaults[i] : line.text.data();
        }
        if (text.empty()) {
            lv_obj_add_flag(diagnostic_label_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_label_set_text(diagnostic_label_, text.c_str());
        lv_obj_align(diagnostic_label_, show_all ? LV_ALIGN_CENTER : LV_ALIGN_BOTTOM_MID,
                     0, show_all ? 0 : -21);
        lv_obj_remove_flag(diagnostic_label_, LV_OBJ_FLAG_HIDDEN);
    }

    void SetDiagnosticLine(DiagnosticChannel channel, const char* message, bool fault) {
        std::lock_guard lock(diagnostic_mutex_);
        auto& line = diagnostics_[static_cast<size_t>(channel)];
        if (line.fault == fault && std::strcmp(line.text.data(), message) == 0) return;
        std::snprintf(line.text.data(), line.text.size(), "%s", message);
        line.fault = fault;
        diagnostic_dirty_ = true;
    }

    static void SetSubtitleY(void* object, int32_t value) {
        lv_obj_set_y(static_cast<lv_obj_t*>(object), value);
    }

    static void SubtitleScrollTimerCallback(lv_timer_t* timer) {
        auto* display = static_cast<NoEmojiSpiLcdDisplay*>(lv_timer_get_user_data(timer));
        if (display != nullptr) {
            display->AdvanceSubtitleLine();
        }
    }

    void StopSubtitleScrolling() {
        if (subtitle_scroll_timer_ != nullptr) {
            lv_timer_pause(subtitle_scroll_timer_);
        }
        if (chat_message_label_ != nullptr) {
            lv_anim_delete(chat_message_label_, SetSubtitleY);
        }
        subtitle_scroll_offset_ = 0;
        subtitle_scroll_max_offset_ = 0;
    }

    void ConfigureSubtitleScrolling(int center_offset_y = 0) {
        StopSubtitleScrolling();
        if (chat_message_label_ == nullptr || subtitle_viewport_ == nullptr) {
            return;
        }

        lv_obj_set_width(chat_message_label_, kSubtitleTextWidth);
        lv_obj_set_height(chat_message_label_, LV_SIZE_CONTENT);
        lv_obj_align(chat_message_label_, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_update_layout(chat_message_label_);

        const lv_font_t* font = lv_obj_get_style_text_font(chat_message_label_, LV_PART_MAIN);
        const int line_space = lv_obj_get_style_text_line_space(chat_message_label_, LV_PART_MAIN);
        subtitle_line_height_ = font != nullptr ? font->line_height + line_space : 25;
        const int text_height = lv_obj_get_height(chat_message_label_);
        const int line_count = std::max(
            1, (text_height + line_space + subtitle_line_height_ - 1) /
                   subtitle_line_height_);
        subtitle_scroll_max_offset_ =
            std::max(0, line_count - kSubtitleVisibleLines) * subtitle_line_height_;

        if (subtitle_scroll_max_offset_ == 0) {
            lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, center_offset_y);
            return;
        }
        if (subtitle_scroll_timer_ == nullptr) {
            return;
        }

        lv_timer_set_period(subtitle_scroll_timer_, kSubtitleScrollIntervalMs);
        lv_timer_reset(subtitle_scroll_timer_);
        lv_timer_resume(subtitle_scroll_timer_);
    }

    void AdvanceSubtitleLine() {
        if (!chat_message_visible_ || chat_message_label_ == nullptr ||
            subtitle_scroll_max_offset_ <= 0) {
            if (subtitle_scroll_timer_ != nullptr) {
                lv_timer_pause(subtitle_scroll_timer_);
            }
            return;
        }

        lv_anim_delete(chat_message_label_, SetSubtitleY);
        if (subtitle_scroll_offset_ >= subtitle_scroll_max_offset_) {
            subtitle_scroll_offset_ = 0;
            lv_obj_set_y(chat_message_label_, 0);
            return;
        }

        const int next_offset = std::min(
            subtitle_scroll_offset_ + subtitle_line_height_, subtitle_scroll_max_offset_);
        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, chat_message_label_);
        lv_anim_set_values(&animation, -subtitle_scroll_offset_, -next_offset);
        lv_anim_set_duration(&animation, kSubtitleScrollAnimationMs);
        lv_anim_set_exec_cb(&animation, SetSubtitleY);
        lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
        lv_anim_start(&animation);
        subtitle_scroll_offset_ = next_offset;
    }

public:
    NoEmojiSpiLcdDisplay(const RndisBoard& rndis_board, esp_lcd_panel_io_handle_t panel_io,
                         esp_lcd_panel_handle_t panel,
                         int width, int height, int offset_x, int offset_y,
                         bool mirror_x, bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                        mirror_x, mirror_y, swap_xy),
          rndis_board_(rndis_board) {
        DisplayLockGuard lock(this);

        if (chat_message_label_ != nullptr) {
            const lv_font_t* font =
                lv_obj_get_style_text_font(chat_message_label_, LV_PART_MAIN);
            const int line_space =
                lv_obj_get_style_text_line_space(chat_message_label_, LV_PART_MAIN);
            const int font_height = font != nullptr ? font->line_height : 25;
            subtitle_line_height_ = font_height + line_space;
            subtitle_viewport_height_ =
                font_height * kSubtitleVisibleLines +
                line_space * (kSubtitleVisibleLines - 1);
            ESP_LOGI(BOARD_TAG,
                     "Subtitle layout: lines=%d line_height=%d viewport_height=%d",
                     kSubtitleVisibleLines, subtitle_line_height_,
                     subtitle_viewport_height_);
        }

        if (emoji_box_ != nullptr && emoji_label_ != nullptr && emoji_image_ != nullptr) {
            lv_image_set_src(emoji_image_, &jiuchuan_logo);
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }

        if (bottom_bar_ != nullptr) {
            lv_obj_set_size(
                bottom_bar_, kSubtitlePanelWidth,
                subtitle_viewport_height_ + kSubtitlePanelPadding * 2);
            lv_obj_set_style_min_height(bottom_bar_, 0, 0);
            lv_obj_set_style_pad_all(bottom_bar_, kSubtitlePanelPadding, 0);
            lv_obj_set_style_radius(bottom_bar_, 6, 0);
            lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
            lv_obj_align(bottom_bar_, LV_ALIGN_CENTER, 0, 0);
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }

        if (bottom_bar_ != nullptr && chat_message_label_ != nullptr) {
            subtitle_viewport_ = lv_obj_create(bottom_bar_);
            lv_obj_set_size(
                subtitle_viewport_, kSubtitleTextWidth, subtitle_viewport_height_);
            lv_obj_set_style_bg_opa(subtitle_viewport_, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(subtitle_viewport_, 0, 0);
            lv_obj_set_style_pad_all(subtitle_viewport_, 0, 0);
            lv_obj_set_scrollbar_mode(subtitle_viewport_, LV_SCROLLBAR_MODE_OFF);
            lv_obj_remove_flag(subtitle_viewport_, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(subtitle_viewport_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
            lv_obj_center(subtitle_viewport_);

            lv_obj_set_parent(chat_message_label_, subtitle_viewport_);
            lv_obj_set_width(chat_message_label_, kSubtitleTextWidth);
            lv_obj_set_height(chat_message_label_, LV_SIZE_CONTENT);
            lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);

            subtitle_scroll_timer_ = lv_timer_create(
                SubtitleScrollTimerCallback, kSubtitleScrollIntervalMs, this);
            if (subtitle_scroll_timer_ != nullptr) {
                lv_timer_pause(subtitle_scroll_timer_);
            } else {
                ESP_LOGE(BOARD_TAG, "Failed to create subtitle scroll timer");
            }
        }

        content_footer_label_ = lv_label_create(lv_screen_active());
        if (content_footer_label_ != nullptr) {
            lv_obj_set_width(content_footer_label_, kSubtitlePanelWidth);
            lv_label_set_long_mode(content_footer_label_, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(content_footer_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(content_footer_label_, lv_color_white(), 0);
            if (chat_message_label_ != nullptr) {
                lv_obj_set_style_text_font(
                    content_footer_label_,
                    lv_obj_get_style_text_font(chat_message_label_, LV_PART_MAIN), 0);
            }
            lv_obj_align(content_footer_label_, LV_ALIGN_BOTTOM_MID, 0, -21);
            lv_obj_add_flag(content_footer_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (network_label_) lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
        if (battery_label_) lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
        if (status_label_) lv_obj_set_width(status_label_, width_ - 92);
        if (notification_label_) lv_obj_set_width(notification_label_, width_ - 92);
        guide_pages_ = std::make_unique<GuidePages>(lv_screen_active(), width_, height_,
            lv_obj_get_style_text_font(chat_message_label_, LV_PART_MAIN));
        navigation_ = std::make_unique<NavigationOverlay>(lv_screen_active(), width_, height_);
        InitDiagnostics();
    }

    ~NoEmojiSpiLcdDisplay() override {
        DisplayLockGuard lock(this);
        if (startup_timer_ != nullptr) {
            lv_timer_delete(startup_timer_);
        }
        if (startup_overlay_ != nullptr) {
            lv_obj_delete(startup_overlay_);
        }
        navigation_.reset();
        guide_pages_.reset();
        lv_obj_delete(diagnostic_label_);
        StopSubtitleScrolling();
        if (subtitle_scroll_timer_ != nullptr) {
            lv_timer_delete(subtitle_scroll_timer_);
            subtitle_scroll_timer_ = nullptr;
        }
    }

    void SetTheme(Theme* theme) override {
        SpiLcdDisplay::SetTheme(theme);
        DisplayLockGuard lock(this);
        if (guide_pages_) guide_pages_->SetFont(
            lv_obj_get_style_text_font(chat_message_label_, LV_PART_MAIN));
    }

    void SetGuideView(const GuideView& view) override {
        DisplayLockGuard lock(this);
        guide_view_ = view;
        if (content_footer_label_ && view.page != GuidePage::Legacy)
            lv_obj_add_flag(content_footer_label_, LV_OBJ_FLAG_HIDDEN);
        ApplyGuideView();
    }

    void ApplyGuideView() {
        GuideView view = guide_view_;
        if (view.page == GuidePage::Narration && view.number.empty())
            view.number = navigation_status_.scene;
        if (guide_pages_) guide_pages_->SetView(view);
    }

    void ShowNotification(const char* text, int duration_ms = 3000) override {
        SpiLcdDisplay::ShowNotification(text, duration_ms);
        DisplayLockGuard lock(this);
        if (guide_pages_) guide_pages_->Notify(text, duration_ms);
    }

    void ShowNotification(const std::string& text, int duration_ms = 3000) override {
        ShowNotification(text.c_str(), duration_ms);
    }

    void SetChatMessage(const char* role, const char* content) override {
        (void)role;
        DisplayLockGuard lock(this);
        if (chat_message_label_ == nullptr) {
            return;
        }

        startup_label_visible_ = content != nullptr &&
            std::strcmp(content, "Vanex S3") == 0;
        if (startup_label_visible_ && startup_qr_image_) {
            content = "";
        }
        const bool has_text = content != nullptr && content[0] != '\0';
        chat_message_visible_ = has_text && !hide_subtitle_;
        lv_label_set_text(chat_message_label_, has_text ? content : "");

        if (chat_message_visible_) {
            if (emoji_box_ != nullptr) {
                lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            }
            if (bottom_bar_ != nullptr) {
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
            const int center_offset_y =
                std::strcmp(content, "Vanex S3") == 0 ? kStartupLabelOffsetY : 0;
            ConfigureSubtitleScrolling(center_offset_y);
            return;
        }

        StopSubtitleScrolling();
        if (bottom_bar_ != nullptr) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_box_ != nullptr &&
            (preview_image_ == nullptr || lv_obj_has_flag(preview_image_, LV_OBJ_FLAG_HIDDEN))) {
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void SetStartupQrImage(std::unique_ptr<LvglImage> image, const std::string& imei) {
        DisplayLockGuard lock(this);
        if (!image || startup_finished_ || startup_overlay_ == nullptr) {
            return;
        }
        const auto* descriptor = image->image_dsc();
        if (descriptor->header.w == 0 || descriptor->header.h == 0) {
            return;
        }

        // Keep the complete QR quiet zone above the IMEI label.
        const int edge = std::min(width_ - 8, height_ - 36);
        const int scale = std::max(1, std::min(
            edge * 256 / static_cast<int>(descriptor->header.w),
            edge * 256 / static_cast<int>(descriptor->header.h)));
        lv_image_set_src(startup_qr_widget_, descriptor);
        startup_qr_image_ = std::move(image);
        lv_image_set_scale(startup_qr_widget_, scale);
        lv_image_set_antialias(startup_qr_widget_, false);
        lv_label_set_text(startup_imei_label_, imei.c_str());
        lv_obj_remove_flag(startup_overlay_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(startup_overlay_);
        // Count from image readiness, never from the beginning of a download.
        lv_timer_set_period(startup_timer_, 3000);
        lv_timer_reset(startup_timer_);
        lv_timer_resume(startup_timer_);
        ESP_LOGI(BOARD_TAG, "[BOOT_SPLASH] QR and IMEI visible for 3000 ms");
    }

    void BeginStartupPresentation(std::function<void()> completion) {
        DisplayLockGuard lock(this);
        startup_completion_ = std::move(completion);
        startup_overlay_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(startup_overlay_, width_, height_);
        lv_obj_center(startup_overlay_);
        lv_obj_set_style_pad_all(startup_overlay_, 0, 0);
        lv_obj_set_style_border_width(startup_overlay_, 0, 0);
        lv_obj_set_style_radius(startup_overlay_, 0, 0);
        lv_obj_set_style_bg_color(startup_overlay_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(startup_overlay_, LV_OPA_COVER, 0);
        lv_obj_remove_flag(startup_overlay_, LV_OBJ_FLAG_SCROLLABLE);

        const int edge = std::min(width_ - 8, height_ - 36);
        auto* image_box = lv_obj_create(startup_overlay_);
        lv_obj_set_size(image_box, edge, edge);
        lv_obj_align(image_box, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_set_style_pad_all(image_box, 0, 0);
        lv_obj_set_style_border_width(image_box, 0, 0);
        lv_obj_set_style_bg_opa(image_box, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(image_box, LV_OBJ_FLAG_SCROLLABLE);
        startup_qr_widget_ = lv_image_create(image_box);
        lv_image_set_src(startup_qr_widget_, &jiuchuan_logo);
        lv_obj_center(startup_qr_widget_);

        startup_imei_label_ = lv_label_create(startup_overlay_);
        lv_label_set_text(startup_imei_label_, "");
        lv_obj_set_style_text_font(startup_imei_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(startup_imei_label_, lv_color_black(), 0);
        lv_obj_align(startup_imei_label_, LV_ALIGN_BOTTOM_MID, 0, -4);

        // There is no deadline measured from power-on: a first boot may need
        // to download the image. Only actual image display starts this timer.
        startup_timer_ = lv_timer_create([](lv_timer_t* timer) {
            auto* display = static_cast<NoEmojiSpiLcdDisplay*>(lv_timer_get_user_data(timer));
            display->FinishStartupPresentation();
        }, 3000, this);
        if (startup_timer_ == nullptr) FinishStartupPresentation();
        else lv_timer_pause(startup_timer_);
    }

    void ShowStartupInitializing() {
        DisplayLockGuard lock(this);
        if (startup_overlay_ != nullptr) lv_obj_add_flag(startup_overlay_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    }

    void FinishStartupPresentation() {
        DisplayLockGuard lock(this);
        if (startup_finished_) return;
        startup_finished_ = true;
        if (startup_timer_ != nullptr) {
            lv_timer_delete(startup_timer_);
            startup_timer_ = nullptr;
        }
        lv_obj_delete(startup_overlay_);
        startup_overlay_ = nullptr;
        startup_qr_widget_ = nullptr;
        startup_imei_label_ = nullptr;
        startup_qr_image_.reset();
        startup_label_visible_ = false;
        chat_message_visible_ = false;
        StopSubtitleScrolling();
        lv_label_set_text(chat_message_label_, "");
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
        ESP_LOGI(BOARD_TAG, "[BOOT_SPLASH] finished; continuing business initialization");
        if (startup_completion_) startup_completion_();
    }

    void SetPreviewImage(std::unique_ptr<LvglImage> image) override {
        DisplayLockGuard lock(this);
        if (guide_pages_) guide_pages_->SetImage(nullptr);
        const bool clearing_preview = image == nullptr;
        LcdDisplay::SetPreviewImage(std::move(image));
        if (guide_pages_ && preview_image_cached_)
            guide_pages_->SetImage(preview_image_cached_->image_dsc());

        if (!clearing_preview && preview_timer_ != nullptr) {
            esp_timer_stop(preview_timer_);
        }

        if (clearing_preview && chat_message_visible_) {
            DisplayLockGuard lock(this);
            if (emoji_box_ != nullptr) {
                lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    void SetContentFooter(const char* footer) override {
        DisplayLockGuard lock(this);
        if (content_footer_label_ == nullptr) {
            return;
        }
        const bool visible = footer != nullptr && footer[0] != '\0' &&
            guide_view_.page == GuidePage::Legacy;
        lv_label_set_text(content_footer_label_, visible ? footer : "");
        if (visible) {
            lv_obj_remove_flag(content_footer_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(content_footer_label_);
            if (bottom_bar_ != nullptr && chat_message_visible_) {
                lv_obj_align(bottom_bar_, LV_ALIGN_CENTER, 0, -8);
            }
        } else {
            lv_obj_add_flag(content_footer_label_, LV_OBJ_FLAG_HIDDEN);
            if (bottom_bar_ != nullptr) {
                lv_obj_align(bottom_bar_, LV_ALIGN_CENTER, 0, 0);
            }
        }
    }

    void SetNavigationStatus(const NavigationStatus& status) override {
        DisplayLockGuard lock(this);
        navigation_status_ = status;
        if (navigation_) navigation_->SetPosition(status);
        ApplyGuideView();
    }

    void SetDiagnostic(DiagnosticChannel channel, const char* message) override {
        SetDiagnosticLine(channel, message, true);
    }

    void SetDiagnosticStatus(DiagnosticChannel channel, const char* message) override {
        SetDiagnosticLine(channel, message, false);
    }

    bool IsDiagnosticPageOpen() const { return diagnostic_page_open_.load(); }

    void ToggleDiagnosticPage() {
        DisplayLockGuard lock(this);
        const bool open = !IsDiagnosticPageOpen();
        diagnostic_page_open_ = open;
        RefreshDiagnostics(true);
        if (open && !startup_qr_image_) lv_obj_move_foreground(diagnostic_label_);
    }

    void UpdateStatusBar(bool update_all = false) override {
        // Preserve the existing low-battery warning and device status updates.
        SpiLcdDisplay::UpdateStatusBar(update_all);
        int level = -1;
        bool charging = false, discharging = false;
        if (!Board::GetInstance().GetBatteryLevel(level, charging, discharging)) level = -1;
        DisplayLockGuard lock(this);
        RefreshDiagnostics();
        if (navigation_) {
            navigation_->SetSignal(rndis_board_.GetCellularSignal());
            navigation_->SetBattery(level);
        }
        if (guide_pages_ && low_battery_popup_ &&
            !lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN))
            guide_pages_->Notify("电量低 请充电", 1500);
    }

    void SetEmotion(const char* emotion) override {
        // Keep the startup QR (or customer logo) through server-side emotions.
    }
};

class JiuchuanDevBoard : public RndisBoard {
private:
    bool cellular_network_ = true;
    std::atomic<bool> startup_pending_{true};
    bool modem_missing_ = false;
    bool startup_network_started_ = false;
    std::atomic<bool> startup_transport_started_{false};
    std::atomic<bool> startup_network_ready_{false};
    startup::Flow startup_flow_;
    std::unique_ptr<WifiBoard> wifi_board_;
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    Button replay_button_;
    Button voice_button_;
    LcdDisplay* display_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    PowerManager* power_manager_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    sdmmc_card_t* sd_card_ = nullptr;
    TaskHandle_t hall_task_handle_ = nullptr;
    TaskHandle_t tf_init_task_handle_ = nullptr;
    TaskHandle_t tf_qr_task_handle_ = nullptr;
    std::unique_ptr<LocationMqttClient> location_mqtt_client_;
    std::unique_ptr<CircularStrip> external_rgb_;
    std::mutex rgb_mutex_;

    int64_t voice_button_down_time_us_ = 0;
    bool voice_button_is_down_ = false;
    bool voice_button_started_listening_ = false;

    static constexpr uint16_t kVoiceButtonHoldStartMs = 300;

    static std::string TrimNetworkValue(std::string value) {
        auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
        value.erase(value.begin(),
                    std::find_if(value.begin(), value.end(),
                                 [&](unsigned char ch) { return !is_space(ch); }));
        value.erase(std::find_if(value.rbegin(), value.rend(),
                                 [&](unsigned char ch) { return !is_space(ch); }).base(),
                    value.end());
        return value;
    }

    static std::string UpperNetworkValue(std::string value) {
        value = TrimNetworkValue(std::move(value));
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::toupper(ch));
                       });
        return value;
    }

    int MapVolumeForDisplay(int internal_volume) const {
        internal_volume = std::max(0, std::min(80, internal_volume));
        return (internal_volume * 100) / 80;
    }

    void InitializeI2c() {
        i2c_master_bus_config_t config = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&config, &codec_i2c_bus_));
        ESP_LOGI(BOARD_TAG, "I2C ready: SDA=GPIO%d SCL=GPIO%d",
                 AUDIO_CODEC_I2C_SDA_PIN, AUDIO_CODEC_I2C_SCL_PIN);
    }

    void InitializeModemPower() {
        if (PWR_EN_GPIO == GPIO_NUM_NC) {
            ESP_LOGI(BOARD_TAG, "ML307 power is managed by the SAM8108 hardware circuit");
            return;
        }
        ESP_LOGI(BOARD_TAG, "Enable ML307/external power: GPIO%d=1", PWR_EN_GPIO);
        GpioManager::Config(PWR_EN_GPIO, GpioManager::GpioMode::OUTPUT);
        GpioManager::SetLevel(PWR_EN_GPIO, 1);
        // USB enumeration is asynchronous. Let the modem boot while the LCD and
        // other local peripherals initialize instead of delaying first light.
        ESP_LOGI(BOARD_TAG, "ML307 stabilization continues asynchronously");
    }

    void InitializeExternalRgbPower() {
        GpioManager::Config(EXTERNAL_LED_ENABLE_GPIO,
                            GpioManager::GpioMode::OUTPUT);
        GpioManager::SetLevel(EXTERNAL_LED_ENABLE_GPIO,
                              !EXTERNAL_LED_ENABLE_LEVEL);
        ESP_LOGI(BOARD_TAG,
                 "External RGB array disabled at startup: data=GPIO%d enable=GPIO%d LEDs=%d max_active=%d",
                 EXTERNAL_LED_GPIO, EXTERNAL_LED_ENABLE_GPIO,
                 EXTERNAL_LED_NUM, EXTERNAL_LED_MAX_ACTIVE);
    }

    void InitializePowerManager() {
        // GPIO4 measures analog battery voltage; it is not a charge-status pin.
        power_manager_ = new PowerManager(GPIO_NUM_NC);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (power_save_timer_) {
                power_save_timer_->SetEnabled(!is_charging);
            }
        });
    }

    void InitializePowerSaveTimer() {
        // Keep the display visible during hardware validation. User activity
        // still resets this timer and restores the saved brightness.
        power_save_timer_ = new PowerSaveTimer(-1, 60, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            if (display_ && static_cast<NoEmojiSpiLcdDisplay*>(display_)->IsDiagnosticPageOpen()) return;
            ESP_LOGI(BOARD_TAG, "Display power save: turning backlight off after inactivity");
            if (display_) {
                display_->SetPowerSaveMode(true);
            }
            GetBacklight()->SetBrightness(0);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(BOARD_TAG, "Display power save: restoring backlight");
            if (display_) {
                display_->SetPowerSaveMode(false);
            }
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGW(BOARD_TAG, "Automatic shutdown disabled; keeping the device awake");
            power_save_timer_->SetEnabled(false);
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeDisplay() {
        ESP_LOGI(BOARD_TAG,
                 "Display wiring: ST7789 SCK=GPIO%d MOSI=GPIO%d DC=GPIO%d CS=GPIO%d RST=GPIO%d BL=GPIO%d invert_bl=%d",
                 DISPLAY_SPI_SCK_PIN, DISPLAY_SPI_MOSI_PIN, DISPLAY_DC_PIN,
                 DISPLAY_SPI_CS_PIN, DISPLAY_RST_PIN, DISPLAY_BACKLIGHT_PIN,
                 DISPLAY_BACKLIGHT_OUTPUT_INVERT);

        spi_bus_config_t bus_config = {};
        bus_config.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        bus_config.miso_io_num = GPIO_NUM_NC;
        bus_config.sclk_io_num = DISPLAY_SPI_SCK_PIN;
        bus_config.quadwp_io_num = GPIO_NUM_NC;
        bus_config.quadhd_io_num = GPIO_NUM_NC;
        bus_config.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus_config, SPI_DMA_CH_AUTO));

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        vTaskDelay(pdMS_TO_TICKS(120));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_LOGI(BOARD_TAG, "LCD panel init complete; applying rotation and mirror");
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(
            panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, DISPLAY_INVERT_COLOR));
        ESP_LOGI(BOARD_TAG, "LCD color inversion applied: %d", DISPLAY_INVERT_COLOR);

        esp_err_t display_on_result = esp_lcd_panel_disp_on_off(panel_, true);
        if (display_on_result != ESP_OK && display_on_result != ESP_ERR_NOT_SUPPORTED) {
            ESP_ERROR_CHECK(display_on_result);
        }

        display_ = new NoEmojiSpiLcdDisplay(
            *this, panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

        auto* backlight = GetBacklight();
        ESP_LOGI(BOARD_TAG,
                 "[LCD_BOOT] ST7789 display initialized: %dx%d, PWM brightness=%u%%",
                 DISPLAY_WIDTH, DISPLAY_HEIGHT,
                 static_cast<unsigned>(backlight->brightness()));
    }

    void InitializeSdCard() {
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_config.clk = SD_PIN_CLK;
        slot_config.cmd = SD_PIN_CMD;
        slot_config.d0 = SD_PIN_D0;
        slot_config.width = 1;
        slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 0,
            .disk_status_check_enable = false,
        };

        esp_err_t result = esp_vfs_fat_sdmmc_mount(
            SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &sd_card_);
        if (result == ESP_OK) {
            ESP_LOGI(BOARD_TAG, "SD card mounted: %s (CLK=GPIO%d CMD=GPIO%d D0=GPIO%d)",
                     SDCARD_MOUNT_POINT, SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0);
        } else {
            ESP_LOGE(BOARD_TAG, "SD card unavailable: %s", esp_err_to_name(result));
            if (GetDisplay()) {
                GetDisplay()->SetDiagnostic(DiagnosticChannel::Qr, "TF MOUNT ERROR");
                GetDisplay()->ShowNotification("内部存储错误", 5000);
            }
        }
    }

    enum class TfAudioState { Ready, NeedsDownload, StorageError };
    enum class TfDownloadResult { Success, Retry, StorageError };
    static constexpr const char* kStartAudioUrl =
        "https://iotossbj.suji.work/SDfile/TF/start.wav";
    static constexpr const char* kStartAudioPath = SDCARD_MOUNT_POINT "/start.wav";
    static constexpr const char* kStartAudioTempPath = SDCARD_MOUNT_POINT "/.start.wav.part";

    TfAudioState CheckStartAudio(const char* path, std::string& error) const {
        error.clear();
        struct stat info = {};
        if (stat(path, &info) != 0) {
            if (errno == ENOENT) {
                error = "start.wav is missing";
                return TfAudioState::NeedsDownload;
            }
            error = "cannot inspect TF audio: errno=" +
                std::to_string(errno) + " (" + std::strerror(errno) + ")";
            return TfAudioState::StorageError;
        }
        if (!S_ISREG(info.st_mode)) {
            error = "TF audio path is not a regular file: " + std::string(path);
            return TfAudioState::StorageError;
        }
        if (info.st_size < 44) {
            error = "start.wav is empty or truncated";
            return TfAudioState::NeedsDownload;
        }

        FILE* file = std::fopen(path, "rb");
        if (!file) {
            error = "cannot read TF audio: errno=" +
                std::to_string(errno) + " (" + std::strerror(errno) + ")";
            return TfAudioState::StorageError;
        }
        unsigned char header[12] = {};
        const size_t read = std::fread(header, 1, sizeof(header), file);
        const bool read_error = std::ferror(file) != 0;
        const int close_result = std::fclose(file);
        if (read_error || close_result != 0) {
            error = "TF audio read/close failed";
            return TfAudioState::StorageError;
        }
        if (read != sizeof(header) || std::memcmp(header, "RIFF", 4) != 0 ||
            std::memcmp(header + 8, "WAVE", 4) != 0) {
            error = "start.wav has an invalid WAV header";
            return TfAudioState::NeedsDownload;
        }
        const uint32_t riff_size = static_cast<uint32_t>(header[4]) |
            (static_cast<uint32_t>(header[5]) << 8) |
            (static_cast<uint32_t>(header[6]) << 16) |
            (static_cast<uint32_t>(header[7]) << 24);
        if (riff_size < 36 || static_cast<uint64_t>(riff_size) + 8 >
                static_cast<uint64_t>(info.st_size)) {
            error = "start.wav WAV length is invalid or incomplete";
            return TfAudioState::NeedsDownload;
        }
        return TfAudioState::Ready;
    }

    TfDownloadResult DownloadInitialStartAudio(std::string& error) {
        error.clear();
        auto* network = GetNetwork();
        if (!network) {
            error = "network is unavailable";
            return TfDownloadResult::Retry;
        }

        auto http = network->CreateHttp(3);
        if (!http) {
            error = "HTTP client creation failed";
            return TfDownloadResult::Retry;
        }
        http->SetTimeout(15000);
        ESP_LOGI(BOARD_TAG, "[TF_INIT] GET %s -> %s", kStartAudioUrl, kStartAudioPath);
        if (!http->Open("GET", kStartAudioUrl)) {
            error = "start.wav HTTP open failed";
            http->Close();
            return TfDownloadResult::Retry;
        }

        const int status = http->GetStatusCode();
        if (status != 200) {
            error = "start.wav HTTP status=" + std::to_string(status);
            http->Close();
            return TfDownloadResult::Retry;
        }

        const size_t expected_size = http->GetBodyLength();
        FILE* file = std::fopen(kStartAudioTempPath, "wb");
        if (!file) {
            error = "cannot open TF temporary file: errno=" +
                std::to_string(errno) + " (" + std::strerror(errno) + ")";
            http->Close();
            return TfDownloadResult::StorageError;
        }

        uint64_t written = 0;
        char buffer[4096];
        TfDownloadResult result = TfDownloadResult::Success;
        while (true) {
            const int read = http->Read(buffer, sizeof(buffer));
            if (read < 0) {
                error = "start.wav HTTP read failed";
                result = TfDownloadResult::Retry;
                break;
            }
            if (read == 0) {
                break;
            }
            if (std::fwrite(buffer, 1, static_cast<size_t>(read), file) !=
                static_cast<size_t>(read)) {
                error = "start.wav TF write failed: errno=" +
                    std::to_string(errno) + " (" + std::strerror(errno) + ")";
                result = TfDownloadResult::StorageError;
                break;
            }
            written += static_cast<uint64_t>(read);
        }
        if (std::fflush(file) != 0 || fsync(fileno(file)) != 0) {
            error = "start.wav TF flush/sync failed: errno=" +
                std::to_string(errno) + " (" + std::strerror(errno) + ")";
            result = TfDownloadResult::StorageError;
        }
        if (std::fclose(file) != 0) {
            error = "start.wav TF close failed: errno=" +
                std::to_string(errno) + " (" + std::strerror(errno) + ")";
            result = TfDownloadResult::StorageError;
        }
        http->Close();

        if (result == TfDownloadResult::Success && expected_size > 0 && written != expected_size) {
            error = "start.wav size mismatch: written=" +
                std::to_string(written) + " expected=" +
                std::to_string(expected_size);
            result = TfDownloadResult::Retry;
        }
        if (result == TfDownloadResult::Success) {
            const auto state = CheckStartAudio(kStartAudioTempPath, error);
            if (state != TfAudioState::Ready) {
                result = state == TfAudioState::StorageError
                    ? TfDownloadResult::StorageError : TfDownloadResult::Retry;
            }
        }
        if (result != TfDownloadResult::Success) {
            std::remove(kStartAudioTempPath);
            return result;
        }
        // FAT rename does not replace a destination. Only remove the old
        // (missing or invalid) file after the new download is flushed and checked.
        if (std::remove(kStartAudioPath) != 0 && errno != ENOENT) {
            error = "cannot replace start.wav on TF: errno=" +
                std::to_string(errno) + " (" + std::strerror(errno) + ")";
            std::remove(kStartAudioTempPath);
            return TfDownloadResult::StorageError;
        }
        if (std::rename(kStartAudioTempPath, kStartAudioPath) != 0) {
            error = "cannot finalize start.wav on TF: errno=" +
                std::to_string(errno) + " (" + std::strerror(errno) + ")";
            std::remove(kStartAudioTempPath);
            return TfDownloadResult::StorageError;
        }
        ESP_LOGI(BOARD_TAG, "[TF_INIT][READY] path=%s bytes=%llu downloaded and verified",
                 kStartAudioPath,
                 static_cast<unsigned long long>(written));
        return TfDownloadResult::Success;
    }

    void ShowInternalStorageError(const std::string& error) {
        ESP_LOGE(BOARD_TAG, "[TF_INIT][ERROR] %s", error.c_str());
        if (GetDisplay()) {
            GetDisplay()->ShowNotification("内部存储错误", 5000);
        }
    }

    void EnsureTfStartAudio() {
        std::string error;
        const auto state = CheckStartAudio(kStartAudioPath, error);
        if (state == TfAudioState::StorageError) {
            GetDisplay()->SetDiagnostic(DiagnosticChannel::Audio, "WAV TF ERROR");
            ShowInternalStorageError(error);
            ResolveStartupAudio();
            return;
        }
        if (state == TfAudioState::Ready) {
            GetDisplay()->SetDiagnosticStatus(DiagnosticChannel::Audio, "WAV READY TF");
            ESP_LOGI(BOARD_TAG, "[TF_INIT][READY] path=%s already valid; skip download",
                     kStartAudioPath);
            Application::GetInstance().NotifyTfStartAudioReady();
            ResolveStartupAudio();
            return;
        }

        ESP_LOGI(BOARD_TAG, "[TF_INIT] path=%s needs download: %s",
                 kStartAudioPath, error.c_str());
        GetDisplay()->SetDiagnostic(DiagnosticChannel::Audio, "WAV WAIT NETWORK");

        // Wait/retry only in this worker, including after a slow modem startup
        // or a prolonged outage. Neither display nor network startup waits here.
        unsigned attempt = 0;
        while (true) {
            if (!startup_transport_started_.load() ||
                (!cellular_network_ && !startup_network_ready_.load())) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            if (cellular_network_ && !IsNetworkReady()) {
                ESP_LOGI(BOARD_TAG, "[TF_INIT] waiting for USB network; download pending");
                if (!WaitForNetworkReady(30000)) {
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    continue;
                }
            }
            const auto result = DownloadInitialStartAudio(error);
            if (result == TfDownloadResult::Success) {
                GetDisplay()->SetDiagnosticStatus(DiagnosticChannel::Audio, "WAV SAVED TF");
                Application::GetInstance().NotifyTfStartAudioReady();
                ResolveStartupAudio();
                return;
            }
            if (result == TfDownloadResult::StorageError) {
                GetDisplay()->SetDiagnostic(DiagnosticChannel::Audio, "WAV TF ERROR");
                ShowInternalStorageError(error);
                ResolveStartupAudio();
                return;
            }
            GetDisplay()->SetDiagnostic(DiagnosticChannel::Audio, "WAV DOWNLOAD RETRY");
            attempt = std::min(attempt + 1, 6U);
            const uint32_t delay_ms = std::min(2000U << (attempt - 1), 60000U);
            ESP_LOGW(BOARD_TAG, "[TF_INIT][RETRY] %s; retry in %lu ms",
                     error.c_str(), static_cast<unsigned long>(delay_ms));
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }

    void StartTfAudioInitialization() {
        if (!sd_card_ || tf_init_task_handle_) {
            return;
        }
        if (xTaskCreate(
                [](void* arg) {
                    auto* board = static_cast<JiuchuanDevBoard*>(arg);
                    board->EnsureTfStartAudio();
                    board->tf_init_task_handle_ = nullptr;
                    vTaskDelete(nullptr);
                },
                "tf_init", 8192, this, 2, &tf_init_task_handle_) != pdPASS) {
            tf_init_task_handle_ = nullptr;
            ShowInternalStorageError("TF initialization task creation failed");
            GetDisplay()->SetDiagnostic(DiagnosticChannel::Audio, "WAV TASK NO MEM");
            ResolveStartupAudio();
        }
    }

    enum class TfQrState { Ready, NeedsDownload, Retry, StorageError };
    static constexpr size_t kMaxStartupQrBytes = 512 * 1024;
    static constexpr const char* kStartupQrBaseUrl =
        "https://iotossbj.suji.work/SDfile/QR/";

    void ReportQrStorageError(std::string& error, const char* operation, int code) {
        char diagnostic[40];
        std::snprintf(diagnostic, sizeof(diagnostic), "QR TF %s E%d", operation, code);
        error = diagnostic;
        GetDisplay()->SetDiagnostic(DiagnosticChannel::Qr, diagnostic);
    }

    TfQrState LoadStartupQr(const std::string& path,
                           std::unique_ptr<LvglImage>& image,
                           std::string& error) {
        image.reset();
        error.clear();
        struct stat info = {};
        if (stat(path.c_str(), &info) != 0) {
            if (errno == ENOENT) {
                error = "QR image is missing";
                return TfQrState::NeedsDownload;
            }
            ReportQrStorageError(error, "STAT", errno);
            return TfQrState::StorageError;
        }
        if (!S_ISREG(info.st_mode)) {
            ReportQrStorageError(error, "PATH", EINVAL);
            return TfQrState::StorageError;
        }
        if (info.st_size < 4 || static_cast<uint64_t>(info.st_size) > kMaxStartupQrBytes) {
            error = "QR JPEG is empty, truncated or too large";
            return TfQrState::NeedsDownload;
        }

        const size_t size = static_cast<size_t>(info.st_size);
        auto* data = static_cast<uint8_t*>(heap_caps_malloc(
            size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!data) {
            data = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
        }
        if (!data) {
            error = "QR JPEG buffer allocation failed";
            return TfQrState::Retry;
        }
        std::unique_ptr<uint8_t, decltype(&heap_caps_free)> jpeg(data, heap_caps_free);
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) {
            ReportQrStorageError(error, "OPEN", errno);
            return TfQrState::StorageError;
        }
        errno = 0;
        const size_t read = std::fread(data, 1, size, file);
        const bool read_error = std::ferror(file) != 0;
        const int read_errno = errno;
        const int close_result = std::fclose(file);
        if (read != size || read_error) {
            ReportQrStorageError(error, "READ", read_errno == 0 ? EIO : read_errno);
            return TfQrState::StorageError;
        }
        if (close_result != 0) {
            ReportQrStorageError(error, "CLOSE", errno);
            return TfQrState::StorageError;
        }
        if (data[0] != 0xff || data[1] != 0xd8 ||
            data[size - 2] != 0xff || data[size - 1] != 0xd9) {
            error = "QR JPEG start/end markers are invalid";
            return TfQrState::NeedsDownload;
        }

        startup_qr::JpegDimensions dimensions;
        if (!startup_qr::ReadJpegDimensions(data, size, dimensions)) {
            error = "QR JPEG header is invalid or dimensions exceed 1024 pixels";
            return TfQrState::NeedsDownload;
        }

        uint8_t* pixels = nullptr;
        size_t pixel_bytes = 0, width = 0, height = 0, stride = 0;
        const esp_err_t result = jpeg_to_image(
            data, size, &pixels, &pixel_bytes, &width, &height, &stride);
        std::unique_ptr<uint8_t, decltype(&heap_caps_free)> decoded(pixels, heap_caps_free);
        if (result != ESP_OK) {
            error = "QR JPEG decode failed: " + std::string(esp_err_to_name(result));
            return result == ESP_ERR_NO_MEM ? TfQrState::Retry : TfQrState::NeedsDownload;
        }
        if (!pixels || width == 0 || height == 0 ||
            width > startup_qr::kMaxJpegDimension || height > startup_qr::kMaxJpegDimension) {
            error = "QR JPEG dimensions must be between 1 and 1024 pixels";
            return TfQrState::NeedsDownload;
        }
        image.reset(new (std::nothrow) LvglAllocatedImage(
            pixels, pixel_bytes, static_cast<int>(width), static_cast<int>(height),
            static_cast<int>(stride), LV_COLOR_FORMAT_RGB565));
        if (!image) {
            error = "QR image descriptor allocation failed";
            return TfQrState::Retry;
        }
        decoded.release();
        return TfQrState::Ready;
    }

    TfDownloadResult DownloadStartupQr(const std::string& url,
                                      const std::string& path,
                                      const std::string& temporary_path,
                                      std::unique_ptr<LvglImage>& image,
                                      std::string& error) {
        auto* network = GetNetwork();
        auto http = network ? network->CreateHttp(4) : nullptr;
        if (!http) {
            error = "QR HTTP client unavailable";
            GetDisplay()->SetDiagnostic(DiagnosticChannel::Qr, "QR HTTP INIT ERROR");
            return TfDownloadResult::Retry;
        }
        http->SetTimeout(15000);
        ESP_LOGI(BOARD_TAG, "[TF_QR] GET %s -> %s", url.c_str(), path.c_str());
        if (!http->Open("GET", url)) {
            error = "QR HTTPS connection failed";
            GetDisplay()->SetDiagnostic(DiagnosticChannel::Qr, "QR HTTPS ERROR");
            http->Close();
            return TfDownloadResult::Retry;
        }
        const int status = http->GetStatusCode();
        if (status != 200) {
            error = "QR HTTP " + std::to_string(status);
            GetDisplay()->SetDiagnostic(DiagnosticChannel::Qr, error.c_str());
            http->Close();
            return TfDownloadResult::Retry;
        }
        const size_t expected_size = http->GetBodyLength();
        if (expected_size > kMaxStartupQrBytes) {
            error = "QR HTTP image exceeds size limit";
            GetDisplay()->SetDiagnostic(DiagnosticChannel::Qr, "QR FILE TOO LARGE");
            http->Close();
            return TfDownloadResult::Retry;
        }
        FILE* file = std::fopen(temporary_path.c_str(), "wb");
        if (!file) {
            ReportQrStorageError(error, "CREATE", errno);
            http->Close();
            return TfDownloadResult::StorageError;
        }

        size_t written = 0;
        char buffer[4096];
        TfDownloadResult result = TfDownloadResult::Success;
        while (true) {
            const int read = http->Read(buffer, sizeof(buffer));
            if (read < 0) {
                error = "QR HTTP read failed";
                GetDisplay()->SetDiagnostic(DiagnosticChannel::Qr, "QR HTTP READ ERROR");
                result = TfDownloadResult::Retry;
                break;
            }
            if (read == 0) {
                break;
            }
            if (written + static_cast<size_t>(read) > kMaxStartupQrBytes) {
                error = "QR HTTP image exceeds size limit";
                GetDisplay()->SetDiagnostic(DiagnosticChannel::Qr, "QR FILE TOO LARGE");
                result = TfDownloadResult::Retry;
                break;
            }
            errno = 0;
            if (std::fwrite(buffer, 1, static_cast<size_t>(read), file) !=
                static_cast<size_t>(read)) {
                ReportQrStorageError(error, "WRITE", errno == 0 ? EIO : errno);
                result = TfDownloadResult::StorageError;
                break;
            }
            written += static_cast<size_t>(read);
        }
        if (result == TfDownloadResult::Success &&
            (std::fflush(file) != 0 || fsync(fileno(file)) != 0)) {
            ReportQrStorageError(error, "SYNC", errno);
            result = TfDownloadResult::StorageError;
        }
        if (std::fclose(file) != 0 && result == TfDownloadResult::Success) {
            ReportQrStorageError(error, "CLOSE", errno);
            result = TfDownloadResult::StorageError;
        }
        http->Close();
        http.reset();

        if (result == TfDownloadResult::Success && expected_size > 0 && written != expected_size) {
            error = "QR HTTP size mismatch";
            GetDisplay()->SetDiagnostic(DiagnosticChannel::Qr, "QR HTTP INCOMPLETE");
            result = TfDownloadResult::Retry;
        }
        if (result == TfDownloadResult::Success) {
            auto state = LoadStartupQr(temporary_path, image, error);
            unsigned decode_attempt = 0;
            while (state == TfQrState::Retry) {
                // The transfer is complete and synced. Memory pressure must
                // not discard it and trigger another cellular download.
                GetDisplay()->SetDiagnostic(DiagnosticChannel::Qr, "QR MEM RETRY");
                decode_attempt = std::min(decode_attempt + 1, 6U);
                const uint32_t delay_ms = std::min(2000U << (decode_attempt - 1), 60000U);
                ESP_LOGW(BOARD_TAG, "[TF_QR][RETRY] %s; retry staged image in %lu ms",
                         error.c_str(), static_cast<unsigned long>(delay_ms));
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
                state = LoadStartupQr(temporary_path, image, error);
            }
            if (state != TfQrState::Ready) {
                result = state == TfQrState::StorageError
                    ? TfDownloadResult::StorageError : TfDownloadResult::Retry;
                if (result == TfDownloadResult::Retry) {
                    GetDisplay()->SetDiagnostic(DiagnosticChannel::Qr, "QR BAD JPEG");
                }
            }
        }
        if (result != TfDownloadResult::Success) {
            std::remove(temporary_path.c_str());
            return result;
        }
        // As for start.wav, FAT needs the old destination removed before rename.
        // Only replace it after a complete download and successful JPEG decode.
        if ((std::remove(path.c_str()) != 0 && errno != ENOENT) ||
            std::rename(temporary_path.c_str(), path.c_str()) != 0) {
            ReportQrStorageError(error, "SAVE", errno);
            std::remove(temporary_path.c_str());
            image.reset();
            return TfDownloadResult::StorageError;
        }
        ESP_LOGI(BOARD_TAG, "[TF_QR][READY] path=%s bytes=%u downloaded and verified",
                 path.c_str(), static_cast<unsigned>(written));
        return TfDownloadResult::Success;
    }

    void ShowStartupQr(std::unique_ptr<LvglImage> image, const std::string& imei) {
        if (!startup_pending_.load()) return;
        static_cast<NoEmojiSpiLcdDisplay*>(display_)->SetStartupQrImage(std::move(image), imei);
        // A slow first download may finish after the screen has gone dark.
        // Restore the backlight on the application task and restart its timeout.
        Application::GetInstance().Schedule([this]() {
            if (power_save_timer_) {
                power_save_timer_->WakeUp();
            }
        });
    }

    std::string GetStartupImei() {
        const std::string live = GetImei();
        if (ml307_at::IsImei(live)) return live;
#ifdef JIUCHUAN_STARTUP_QR_IMEI
        const std::string configured = JIUCHUAN_STARTUP_QR_IMEI;
        if (ml307_at::IsImei(configured)) return configured;
#endif
        // Used only to find a local startup image, never for business identity.
        Settings settings("device", false);
        const std::string saved = settings.GetString("imei");
        return ml307_at::IsImei(saved) ? saved : std::string{};
    }

    void EnsureTfStartupQr() {
        std::string imei = GetStartupImei();
        bool local_phase = true;
        std::string error;
        unsigned attempt = 0;
        while (true) {
            if (!local_phase) {
                if (!startup_transport_started_.load()) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }
                imei = GetImei();
#ifdef JIUCHUAN_STARTUP_QR_IMEI
                // A device-specific build can download its QR while USB AT
                // recovers. This identity is used only by the QR worker.
                if (!ml307_at::IsImei(imei)) imei = JIUCHUAN_STARTUP_QR_IMEI;
#endif
            }
            const bool has_imei = ml307_at::IsImei(imei);
            if (!has_imei && !local_phase) {
                GetDisplay()->SetDiagnostic(DiagnosticChannel::Qr, "QR WAIT IMEI");
                ESP_LOGI(BOARD_TAG, "[TF_QR] waiting for device IMEI");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            const std::string filename = imei + ".jpg";
            const std::string path = std::string(SDCARD_MOUNT_POINT) + "/" + filename;
            const std::string temporary_path =
                std::string(SDCARD_MOUNT_POINT) + "/." + filename + ".part";
            const std::string url = std::string(kStartupQrBaseUrl) + filename;
            std::unique_ptr<LvglImage> image;
            const auto state = has_imei ? LoadStartupQr(path, image, error) : TfQrState::NeedsDownload;
            if (state == TfQrState::Ready) {
                GetDisplay()->SetDiagnosticStatus(DiagnosticChannel::Qr, "QR READY TF");
                ESP_LOGI(BOARD_TAG, "[TF_QR][READY] local path=%s; skip download", path.c_str());
                ShowStartupQr(std::move(image), imei);
                return;
            }
            if (state == TfQrState::StorageError) {
                ShowInternalStorageError("QR: " + error);
                FailStartupQr();
                return;
            }
            if (state == TfQrState::NeedsDownload) {
                if (local_phase) {
                    local_phase = false;
                    RequestStartupNetwork();
                    continue;
                }
                if (!cellular_network_ && !startup_network_ready_.load()) {
                    GetDisplay()->SetDiagnostic(DiagnosticChannel::Qr, "QR WAIT NETWORK");
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }
                if (cellular_network_ && !IsNetworkReady()) {
                    GetDisplay()->SetDiagnostic(DiagnosticChannel::Qr, "QR WAIT NETWORK");
                    if (!WaitForNetworkReady(30000)) {
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    }
                    // Recheck the live IMEI once DHCP finishes.
                    continue;
                }
                GetDisplay()->SetDiagnostic(DiagnosticChannel::Qr, "QR DOWNLOADING");
                const auto result = DownloadStartupQr(url, path, temporary_path, image, error);
                if (result == TfDownloadResult::Success) {
                    GetDisplay()->SetDiagnosticStatus(DiagnosticChannel::Qr, "QR SAVED TF");
                    ShowStartupQr(std::move(image), imei);
                    return;
                }
                if (result == TfDownloadResult::StorageError) {
                    ShowInternalStorageError("QR: " + error);
                    FailStartupQr();
                    return;
                }
            }
            attempt = std::min(attempt + 1, 6U);
            const uint32_t delay_ms = std::min(2000U << (attempt - 1), 60000U);
            ESP_LOGW(BOARD_TAG, "[TF_QR][RETRY] %s; retry in %lu ms",
                     error.c_str(), static_cast<unsigned long>(delay_ms));
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }

    void StartTfQrInitialization() {
        if (!sd_card_ || tf_qr_task_handle_) {
            return;
        }
        if (xTaskCreate(
                [](void* arg) {
                    auto* board = static_cast<JiuchuanDevBoard*>(arg);
                    board->EnsureTfStartupQr();
                    board->tf_qr_task_handle_ = nullptr;
                    vTaskDelete(nullptr);
                },
                "tf_qr", 8192, this, 2, &tf_qr_task_handle_) != pdPASS) {
            tf_qr_task_handle_ = nullptr;
            ESP_LOGE(BOARD_TAG, "[TF_QR][ERROR] QR initialization task creation failed");
            GetDisplay()->SetDiagnostic(DiagnosticChannel::Qr, "QR TASK NO MEM");
            FailStartupQr();
        }
    }

    void InitializeHallSensor() {
        gpio_config_t config = {
            .pin_bit_mask = 1ULL << HALL_PIN,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&config));

        xTaskCreate([](void*) {
            int previous_level = gpio_get_level(HALL_PIN);
            ESP_LOGI(BOARD_TAG, "Hall sensor initial level=%d", previous_level);
            while (true) {
                vTaskDelay(pdMS_TO_TICKS(100));
                int level = gpio_get_level(HALL_PIN);
                if (level != previous_level) {
                    ESP_LOGI(BOARD_TAG, "Hall sensor level=%d", level);
                    previous_level = level;
                }
            }
        }, "hall_sensor", 2048, nullptr, 3, &hall_task_handle_);
    }

    void StartBusinessMqtt() {
        if (startup_network_started_ && !location_mqtt_client_) {
            location_mqtt_client_ = std::make_unique<LocationMqttClient>(*this, [this]() {
                return startup_network_ready_.load();
            });
            location_mqtt_client_->Start();
        }
    }

    void CompleteStartupPresentation() {
        ApplyStartupActions(startup_flow_.QrPresentationFinished());
    }

    void ApplyStartupActions(const startup::Actions& actions) {
        if (actions.start_network) {
            ESP_LOGI(BOARD_TAG, "[BOOT_FLOW] local startup phase completed; starting transport");
            startup_network_started_ = true;
            static_cast<NoEmojiSpiLcdDisplay*>(display_)->ShowStartupInitializing();
            if (cellular_network_) RndisBoard::StartNetwork();
            else if (wifi_board_) wifi_board_->StartNetwork();
            startup_transport_started_ = true;
        }
        if (actions.start_business && startup_pending_.exchange(false)) {
            ESP_LOGI(BOARD_TAG, "[BOOT_FLOW] QR presentation and audio preparation resolved");
            StartBusinessMqtt();
            Application::GetInstance().NotifyStartupPresentationDone();
        }
    }

    void RequestStartupNetwork() {
        Application::GetInstance().Schedule([this]() {
            ApplyStartupActions(startup_flow_.LocalQrUnavailable());
        });
    }

    void ResolveStartupAudio() {
        Application::GetInstance().Schedule([this]() {
            ApplyStartupActions(startup_flow_.AudioResolved());
        });
    }

    void FailStartupQr() {
        Application::GetInstance().Schedule([this]() {
            static_cast<NoEmojiSpiLcdDisplay*>(display_)->FinishStartupPresentation();
        });
    }

    void ChangeVolume(int delta) {
        auto codec = GetAudioCodec();
        int volume = std::max(0, std::min(80, codec->output_volume() + delta));
        codec->SetOutputVolume(volume);
        power_save_timer_->WakeUp();

        if (volume == 0) {
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        } else {
            GetDisplay()->ShowNotification(
                Lang::Strings::VOLUME + std::to_string(MapVolumeForDisplay(volume)) + "%");
        }
    }

    void InitializeButtons() {
        ESP_LOGI(BOARD_TAG,
                 "Buttons: role=GPIO%d volume_up=GPIO%d volume_down=GPIO%d replay=GPIO%d voice=GPIO%d",
                 BOOT_BUTTON_GPIO, WIFI_BUTTON_GPIO, CMD_BUTTON_GPIO,
                 REPLAY_BUTTON_GPIO, VOICE_BUTTON_GPIO);

        boot_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            ESP_LOGI(BOARD_TAG, "Role switch button clicked");
            Application::GetInstance().SwitchRoleVoiceBinding();
        });

        replay_button_.OnPressDown([this]() {
            power_save_timer_->WakeUp();
        });
        replay_button_.OnLongPress([this]() {
            Application::GetInstance().Schedule([this]() {
                power_save_timer_->WakeUp();
                static_cast<NoEmojiSpiLcdDisplay*>(display_)->ToggleDiagnosticPage();
            });
        });
        replay_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            if (location_mqtt_client_) {
                location_mqtt_client_->ReplayContent(false);
            }
        });
        replay_button_.OnDoubleClick([this]() {
            power_save_timer_->WakeUp();
            if (location_mqtt_client_) {
                location_mqtt_client_->ReplayContent(true);
            }
        });

        volume_up_button_.OnPressDown([this]() {
            ChangeVolume(8);
        });
        volume_down_button_.OnPressDown([this]() {
            ChangeVolume(-8);
        });

        voice_button_.OnPressDown([this]() {
            voice_button_is_down_ = true;
            voice_button_started_listening_ = false;
            voice_button_down_time_us_ = esp_timer_get_time();
            power_save_timer_->WakeUp();
            ESP_LOGI(BOARD_TAG,
                     "Voice button pressed: interrupt current playback immediately");
            Application::GetInstance().InterruptCurrentPlayback();
        });
        voice_button_.OnLongPress([this]() {
            if (!voice_button_is_down_ || voice_button_started_listening_) {
                return;
            }
            voice_button_started_listening_ = true;
            ESP_LOGI(BOARD_TAG, "Voice button held: start manual listening");
            Application::GetInstance().StartListening();
        });
        voice_button_.OnPressUp([this]() {
            const int64_t elapsed_ms = voice_button_down_time_us_ == 0
                ? 0
                : (esp_timer_get_time() - voice_button_down_time_us_) / 1000;
            voice_button_is_down_ = false;
            power_save_timer_->WakeUp();

            if (voice_button_started_listening_) {
                ESP_LOGI(BOARD_TAG, "Voice button released after %lld ms: stop manual listening",
                         static_cast<long long>(elapsed_ms));
                Application::GetInstance().StopListening();
            } else {
                ESP_LOGI(BOARD_TAG, "Voice button short press: interrupt playback (%lld ms)",
                         static_cast<long long>(elapsed_ms));
                Application::GetInstance().InterruptCurrentPlayback();
            }

            voice_button_started_listening_ = false;
            voice_button_down_time_us_ = 0;
        });
    }

public:
    JiuchuanDevBoard()
        : RndisBoard(PrepareBoardForStartup(), true, UseEarlyUsbPowerOnRetry()),
          boot_button_(BOOT_BUTTON_GPIO),
          volume_up_button_(WIFI_BUTTON_GPIO),
          volume_down_button_(CMD_BUTTON_GPIO),
          replay_button_(REPLAY_BUTTON_GPIO),
          voice_button_(VOICE_BUTTON_GPIO, false, kVoiceButtonHoldStartMs) {
        ESP_LOGI(BOARD_TAG, "Initializing jiuchuan-s3 hardware on XiaoZhi 2.1.0");
        Settings network_settings("network", false);
        cellular_network_ = network_settings.GetInt("type", 1) == 1;
        if (cellular_network_) {
            ESP_LOGI(BOARD_TAG, "Selected network: GSM (ML307 USB RNDIS)");
            InitializeModemPower();
        } else {
            ESP_LOGI(BOARD_TAG, "Selected network: WiFi");
            wifi_board_ = std::make_unique<WifiBoard>();
        }
        InitializeExternalRgbPower();
        InitializeI2c();
        InitializePowerManager();
        InitializeDisplay();
        static_cast<NoEmojiSpiLcdDisplay*>(display_)->BeginStartupPresentation([this]() {
            Application::GetInstance().Schedule([this]() { CompleteStartupPresentation(); });
        });
        ESP_LOGI(BOARD_TAG, "[LCD_BOOT] Startup logo prepared");
        InitializePowerSaveTimer();
        InitializeButtons();
        InitializeHallSensor();
        ESP_LOGI(BOARD_TAG, "jiuchuan-s3 hardware initialization complete");
    }

    ~JiuchuanDevBoard() override {
        if (tf_qr_task_handle_) {
            vTaskDelete(tf_qr_task_handle_);
        }
        location_mqtt_client_.reset();
        if (hall_task_handle_) {
            vTaskDelete(hall_task_handle_);
        }
        delete power_save_timer_;
        delete power_manager_;
        delete display_;
    }

    Led* GetLed() override {
        static CircularStrip led(BUILTIN_LED_GPIO, BUILTIN_LED_NUM);
        return &led;
    }

    AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    Display* GetDisplay() override {
        return display_;
    }

    Backlight* GetBacklight() override {
        static PwmBacklight backlight(
            DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000, true);
        return &backlight;
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (!power_manager_) {
            return false;
        }

        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    bool ApplyRgbControl(
            bool update_rgb1, const std::vector<IndexedRgbColor>& rgb1,
            bool update_rgb2, const std::vector<IndexedRgbColor>& rgb2,
            std::string& error) override {
        std::lock_guard<std::mutex> lock(rgb_mutex_);
        error.clear();

        auto validate = [&error](const char* name,
                                 const std::vector<IndexedRgbColor>& pixels,
                                 size_t led_count, size_t max_active) {
            std::vector<bool> seen(led_count + 1, false);
            size_t active_count = 0;
            for (const auto& pixel : pixels) {
                if (pixel.index == 0 || pixel.index > led_count) {
                    error = std::string(name) + " index must be between 1 and " +
                        std::to_string(led_count);
                    return false;
                }
                if (seen[pixel.index]) {
                    error = std::string(name) + " contains duplicate LED index " +
                        std::to_string(pixel.index);
                    return false;
                }
                seen[pixel.index] = true;
                if (pixel.red != 0 || pixel.green != 0 || pixel.blue != 0) {
                    ++active_count;
                }
            }
            if (active_count > max_active) {
                error = std::string(name) + " supports at most " +
                    std::to_string(max_active) + " active LEDs";
                return false;
            }
            return true;
        };

        if (update_rgb1 && !validate("rgb1", rgb1, BUILTIN_LED_NUM,
                                     BUILTIN_LED_NUM)) {
            return false;
        }
        if (update_rgb2 && !validate("rgb2", rgb2, EXTERNAL_LED_NUM,
                                     EXTERNAL_LED_MAX_ACTIVE)) {
            return false;
        }

        std::vector<StripColor> rgb1_colors;
        size_t rgb1_active = 0;
        if (update_rgb1) {
            rgb1_colors.resize(BUILTIN_LED_NUM);
            for (const auto& pixel : rgb1) {
                rgb1_colors[pixel.index - 1] = {
                    pixel.red, pixel.green, pixel.blue};
                if (pixel.red != 0 || pixel.green != 0 || pixel.blue != 0) {
                    ++rgb1_active;
                }
            }
        }

        std::vector<StripColor> rgb2_colors;
        size_t rgb2_active = 0;
        if (update_rgb2) {
            rgb2_colors.resize(EXTERNAL_LED_NUM);
            for (const auto& pixel : rgb2) {
                rgb2_colors[pixel.index - 1] = {
                    pixel.red, pixel.green, pixel.blue};
                if (pixel.red != 0 || pixel.green != 0 || pixel.blue != 0) {
                    ++rgb2_active;
                }
            }
            if (rgb2_active > 0 && !external_rgb_) {
                external_rgb_ = std::make_unique<CircularStrip>(
                    EXTERNAL_LED_GPIO, EXTERNAL_LED_NUM);
            }
        }

        if (update_rgb2) {
            if (rgb2_active > 0) {
                GpioManager::SetLevel(EXTERNAL_LED_ENABLE_GPIO,
                                      EXTERNAL_LED_ENABLE_LEVEL);
                vTaskDelay(pdMS_TO_TICKS(10));
                external_rgb_->SetManualColors(rgb2_colors);
            } else {
                if (external_rgb_) {
                    external_rgb_->SetManualColors(rgb2_colors);
                    vTaskDelay(pdMS_TO_TICKS(2));
                }
                GpioManager::SetLevel(EXTERNAL_LED_ENABLE_GPIO,
                                      !EXTERNAL_LED_ENABLE_LEVEL);
            }
        }

        if (update_rgb1) {
            static_cast<CircularStrip*>(GetLed())->SetManualColors(rgb1_colors);
        }

        ESP_LOGI(BOARD_TAG,
                 "Indexed RGB applied: rgb1_update=%d rgb1_active=%u rgb2_update=%d rgb2_active=%u",
                 update_rgb1, static_cast<unsigned>(rgb1_active),
                 update_rgb2, static_cast<unsigned>(rgb2_active));
        return true;
    }

    bool RequestShutdown(std::string& error) override {
        error.clear();
        if (!power_manager_) {
            error = "power manager is not ready";
            return false;
        }

        ESP_LOGW(BOARD_TAG,
                 "Remote shutdown accepted: stopping local peripherals before deep sleep");
        if (power_save_timer_) {
            power_save_timer_->SetEnabled(false);
        }
        if (display_) {
            display_->SetPowerSaveMode(true);
        }
        GetBacklight()->SetBrightness(0);

        auto* codec = GetAudioCodec();
        codec->EnableInput(false);
        codec->EnableOutput(false);
        static_cast<CircularStrip*>(GetLed())->SetAllColor({0, 0, 0});
        {
            std::lock_guard<std::mutex> lock(rgb_mutex_);
            if (external_rgb_) {
                external_rgb_->SetAllColor({0, 0, 0});
            }
            GpioManager::SetLevel(EXTERNAL_LED_ENABLE_GPIO,
                                  !EXTERNAL_LED_ENABLE_LEVEL);
        }

        // Backlight fades in 5 ms steps; allow enough time to reach zero.
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_ERROR_CHECK(gpio_hold_en(DISPLAY_BACKLIGHT_PIN));
        gpio_deep_sleep_hold_en();
        power_manager_->SetPowerState(PowerState::SHUTDOWN);

        error = "deep sleep returned unexpectedly";
        return false;
    }

    void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER && power_save_timer_) {
            power_save_timer_->WakeUp();
        }
        if (cellular_network_) {
            RndisBoard::SetPowerSaveLevel(level);
        } else if (wifi_board_) {
            wifi_board_->SetPowerSaveLevel(level);
        }
    }

    void OnUsbAtReady() override {
#if defined(CONFIG_BT_NIMBLE_ROLE_OBSERVER) && !defined(CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING)
        Application::GetInstance().Schedule(StartBleBeaconScan);
#endif
    }

    void StartNetwork() override {
        // The base constructor already reserves the USB host interrupt.
        // Mount TF and try local resources before starting USB/WiFi transport.
        if (!sd_card_) {
            InitializeSdCard();
        }
        if (!sd_card_) {
            ResolveStartupAudio();
            FailStartupQr();
        } else {
            StartTfQrInitialization();
            StartTfAudioInitialization();
        }
#if defined(CONFIG_BT_NIMBLE_ROLE_OBSERVER) && !defined(CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING)
        if (!cellular_network_) StartBleBeaconScan();
        else GetDisplay()->SetDiagnosticStatus(DiagnosticChannel::Ble, "BLE WAIT AT");
#endif
    }

    NetworkInterface* GetNetwork() override {
        return cellular_network_
            ? RndisBoard::GetNetwork()
            : (wifi_board_ ? wifi_board_->GetNetwork() : nullptr);
    }

    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        auto wrapped = [this, callback = std::move(callback)](
                           NetworkEvent event, const std::string& data) {
            if (event == NetworkEvent::Connected) startup_network_ready_ = true;
            if (event == NetworkEvent::Disconnected || event == NetworkEvent::Scanning)
                startup_network_ready_ = false;
            if (!cellular_network_) {
                if (event == NetworkEvent::Connected)
                    display_->SetDiagnosticStatus(DiagnosticChannel::Network, "WIFI IP OK");
                else if (event == NetworkEvent::Disconnected)
                    display_->SetDiagnostic(DiagnosticChannel::Network, "WIFI LINK DOWN");
                else if (event == NetworkEvent::Connecting)
                    display_->SetDiagnosticStatus(DiagnosticChannel::Network, "WIFI WAIT IP");
            }
            if (callback) callback(event, data);
            if (event == NetworkEvent::ModemDetecting && data == "ML307 USB") {
                Application::GetInstance().Schedule([this]() {
                    if (HasUsbModem() || IsNetworkReady() || !startup_pending_.load()) return;
                    if (modem_retry_marker != kModemRetryMarker) {
                        modem_retry_marker = kModemRetryMarker;
                        ESP_LOGW(BOARD_TAG, "[USB_BOOT] Retrying ML307 with early USB port power");
                        esp_restart();
                        return;
                    }
                    modem_missing_ = true;
                    display_->SetStatus("4G USB ERR");
                    display_->SetDiagnostic(DiagnosticChannel::Usb, "USB NO DEVICE");
                });
            } else if (event == NetworkEvent::Connected ||
                       (event == NetworkEvent::Connecting && data == "ML307 USB")) {
                Application::GetInstance().Schedule([this]() {
                    if (modem_missing_ && startup_pending_.load()) {
                        display_->SetStatus(Lang::Strings::INITIALIZING);
                    }
                    modem_missing_ = false;
                });
            }
        };
        if (cellular_network_) {
            RndisBoard::SetNetworkEventCallback(std::move(wrapped));
        } else if (wifi_board_) {
            wifi_board_->SetNetworkEventCallback(std::move(wrapped));
        }
    }

    const char* GetNetworkStateIcon() override {
        return cellular_network_
            ? RndisBoard::GetNetworkStateIcon()
            : (wifi_board_ ? wifi_board_->GetNetworkStateIcon() : "");
    }

    bool IsCellularNetwork() const override {
        return cellular_network_;
    }

    bool RequiresUserBinding() const override { return true; }
    bool IsStartupPresentationPending() const override { return startup_pending_.load(); }

    void OnUserBindingChanged() override {
        if (location_mqtt_client_) {
            location_mqtt_client_->RequestOnlineReport();
        }
    }

    std::string GetDeviceStatusJson() override {
        return cellular_network_
            ? RndisBoard::GetDeviceStatusJson()
            : (wifi_board_ ? wifi_board_->GetDeviceStatusJson() : "{}");
    }

    bool ConfigureNetwork(const std::string& type,
                          const std::string& wifi_credentials,
                          bool apply,
                          bool& reboot_required,
                          std::string& error) override {
        reboot_required = false;
        error.clear();
        const std::string target = UpperNetworkValue(type);

        if (target == "GSM" || target == "4G" || target == "ML307") {
            reboot_required = !cellular_network_;
            if (apply) {
                Settings network_settings("network", true);
                network_settings.SetInt("type", 1);
            }
            ESP_LOGI(BOARD_TAG, "Remote network request accepted: GSM, reboot=%d",
                     reboot_required);
            return true;
        }

        if (target != "WIFI") {
            error = "net must be WIFI or GSM";
            return false;
        }

        const std::string credentials = TrimNetworkValue(wifi_credentials);
        if (credentials.empty()) {
            if (SsidManager::GetInstance().GetSsidList().empty()) {
                error = "WIFIPW is required because no WiFi is saved";
                return false;
            }
            reboot_required = cellular_network_;
            if (apply) {
                Settings network_settings("network", true);
                network_settings.SetInt("type", 0);
            }
            ESP_LOGI(BOARD_TAG,
                     "Remote network request accepted: reuse saved WiFi, reboot=%d",
                     reboot_required);
            return true;
        }

        const size_t separator = credentials.find('|');
        if (separator == std::string::npos ||
            credentials.find('|', separator + 1) != std::string::npos) {
            error = "WIFIPW must use SSID | PASSWORD format";
            return false;
        }
        const std::string ssid =
            TrimNetworkValue(credentials.substr(0, separator));
        const std::string password =
            TrimNetworkValue(credentials.substr(separator + 1));
        if (ssid.empty() || ssid.size() > 32) {
            error = "WiFi SSID must contain 1 to 32 bytes";
            return false;
        }
        if ((!password.empty() && password.size() < 8) || password.size() > 64) {
            error = "WiFi password must be empty or contain 8 to 64 bytes";
            return false;
        }

        auto& ssid_manager = SsidManager::GetInstance();
        bool credentials_changed = true;
        bool already_preferred = false;
        const auto& current_list = ssid_manager.GetSsidList();
        if (!current_list.empty() && current_list.front().ssid == ssid &&
            current_list.front().password == password) {
            already_preferred = true;
        }
        for (const auto& item : current_list) {
            if (item.ssid == ssid && item.password == password) {
                credentials_changed = false;
                break;
            }
        }
        if (apply) {
            ssid_manager.AddSsid(ssid, password);
            const auto& updated_list = ssid_manager.GetSsidList();
            for (size_t index = 0; index < updated_list.size(); ++index) {
                if (updated_list[index].ssid == ssid) {
                    ssid_manager.SetDefaultSsid(static_cast<int>(index));
                    break;
                }
            }
            Settings network_settings("network", true);
            network_settings.SetInt("type", 0);
        }
        reboot_required = cellular_network_ || credentials_changed ||
                          !already_preferred;
        ESP_LOGI(BOARD_TAG,
                 "Remote network request accepted: WiFi SSID=%s, reboot=%d",
                 ssid.c_str(), reboot_required);
        return true;
    }
};

DECLARE_BOARD(JiuchuanDevBoard);
