// Real LVGL software rendering; only the board's locking/scrolling hooks are stubbed.
#include <lvgl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <functional>
#include <mutex>
#include <cstdio>
#include "../main/display/diagnostic_channel.h"
#define BOARD_TAG "test"
#define ESP_LOGI(...) ((void)0)
namespace Lang { namespace Strings { constexpr auto INITIALIZING = "Initializing"; } }
static lv_image_dsc_t jiuchuan_logo{};

class LvglImage {
public:
    std::vector<uint16_t> pixels;
    lv_image_dsc_t descriptor{};
    explicit LvglImage(int size) : pixels(size * size, 0xffff) {
        // Four different edge blocks inside a white quiet zone reveal clipping,
        // mirroring, color conversion and misplaced transform pivots.
        for (int y = 20; y < size - 20; ++y) {
            for (int x = 20; x < size - 20; ++x) {
                pixels[y * size + x] = y < size / 2
                    ? (x < size / 2 ? 0xf800 : 0x07e0)
                    : (x < size / 2 ? 0x001f : 0x0000);
            }
        }
        descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
        descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
        descriptor.header.w = size;
        descriptor.header.h = size;
        descriptor.header.stride = size * 2;
        descriptor.data_size = pixels.size() * 2;
        descriptor.data = reinterpret_cast<const uint8_t*>(pixels.data());
    }
    const lv_image_dsc_t* image_dsc() const { return &descriptor; }
};

class BaseDisplay {
public:
    virtual void SetChatMessage(const char*, const char*) {}
    virtual void SetDiagnostic(DiagnosticChannel, const char*) {}
    virtual void SetDiagnosticStatus(DiagnosticChannel, const char*) {}
};
class TestDisplay : public BaseDisplay {
public:
    using NoEmojiSpiLcdDisplay = TestDisplay;
    lv_obj_t* chat_message_label_;
    lv_obj_t* emoji_box_;
    lv_obj_t* emoji_image_;
    lv_obj_t* emoji_label_;
    lv_obj_t* bottom_bar_;
    lv_obj_t* preview_image_;
    lv_obj_t* status_label_;
    lv_obj_t* startup_overlay_ = nullptr;
    lv_obj_t* startup_qr_widget_ = nullptr;
    lv_obj_t* startup_imei_label_ = nullptr;
    lv_timer_t* startup_timer_ = nullptr;
    bool startup_finished_ = false;
    std::function<void()> startup_completion_;
    bool startup_label_visible_ = false, chat_message_visible_ = false, hide_subtitle_ = false;
    int width_ = 240, height_ = 135;
    std::unique_ptr<LvglImage> startup_qr_image_;
    static constexpr int kStartupLabelOffsetY = 25;
    struct DisplayLockGuard { explicit DisplayLockGuard(TestDisplay*) {} };
    TestDisplay() {
        auto* screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x808080), 0);
        emoji_box_ = lv_obj_create(screen);
        lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(emoji_box_, 0, 0);
        lv_obj_set_style_border_width(emoji_box_, 0, 0);
        lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
        lv_obj_center(emoji_box_);
        emoji_label_ = lv_label_create(emoji_box_);
        emoji_image_ = lv_image_create(emoji_box_);
        lv_obj_center(emoji_image_);
        preview_image_ = lv_image_create(screen);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        bottom_bar_ = lv_obj_create(screen);
        chat_message_label_ = lv_label_create(bottom_bar_);
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        status_label_ = lv_label_create(screen);
        InitDiagnostics();
    }
    void StopSubtitleScrolling() {}
    void ConfigureSubtitleScrolling(int) {}
#include "tf_qr_display_production.inc"
#include "diagnostics_production.inc"
};

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static std::array<uint16_t, 240 * 135> framebuffer;
static std::array<uint16_t, 240 * 135> buffer;
static void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    auto* source = reinterpret_cast<uint16_t*>(pixels);
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            framebuffer[y * 240 + x] = *source++;
        }
    }
    lv_display_flush_ready(display);
}

int main() {
    lv_init();
    auto* display = lv_display_create(240, 135);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer.data(), nullptr, buffer.size() * 2, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, Flush);
    TestDisplay board;
    int completed = 0;
    board.BeginStartupPresentation([&]() { ++completed; });
    board.ShowStartupInitializing();
    board.SetDiagnostic(DiagnosticChannel::Usb, "USB NO DEVICE");
    board.SetDiagnostic(DiagnosticChannel::Qr, "QR WAIT IMEI");
    board.RefreshDiagnostics();
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(display);
    const auto pixel = [](int x, int y) { return framebuffer[y * 240 + x]; };
    Check(std::strcmp(lv_label_get_text(board.diagnostic_label_), "USB NO DEVICE\nQR WAIT IMEI") == 0,
          "the USB and QR explanations should both be visible");
    Check(pixel(7, 112) == 0, "diagnostics should render above the bottom status icons");
    board.SetStartupQrImage(std::make_unique<LvglImage>(300), "864479086534998");
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(display);
    Check(lv_image_get_transformed_width(board.startup_qr_widget_) <= 99, "QR width exceeds viewport");
    Check(lv_image_get_transformed_height(board.startup_qr_widget_) <= 99, "QR height exceeds viewport");
    Check(pixel(7, 112) == 0xffff, "diagnostics cover the QR");
    Check(pixel(85, 26) == 0xf800, "top-left QR content clipped or red byte order incorrect");
    Check(pixel(155, 26) == 0x07e0, "top-right QR content clipped or green byte order incorrect");
    Check(pixel(85, 81) == 0x001f, "bottom-left QR content clipped or blue byte order incorrect");
    Check(pixel(155, 81) == 0x0000, "bottom-right QR content clipped");
    Check(pixel(72, 50) == 0xffff && pixel(167, 50) == 0xffff, "horizontal QR quiet zone clipped");
    Check(pixel(120, 8) == 0xffff && pixel(120, 103) == 0xffff, "vertical QR quiet zone clipped");
    Check(lv_obj_get_y(lv_obj_get_parent(board.startup_qr_widget_)) == 6, "QR top margin missing");
    Check(lv_obj_get_child_count(board.startup_overlay_) == 2, "startup page should contain only image and IMEI");
    Check(std::strcmp(lv_label_get_text(board.startup_imei_label_), "864479086534998") == 0, "IMEI not displayed");
    Check(lv_obj_get_y(board.startup_imei_label_) >= 105, "IMEI overlaps QR");
    Check(lv_obj_get_y(board.startup_imei_label_) + lv_obj_get_height(board.startup_imei_label_) == 131,
          "IMEI is not aligned near the bottom");
    std::cout << "PASS real LVGL: full QR, white quiet zone, RGB565 and IMEI without firmware version\n";
    board.SetChatMessage("assistant", "initialization message");
    lv_refr_now(display);
    Check(pixel(85, 26) == 0xf800, "initialization text covers splash");
    lv_tick_inc(2999); lv_timer_handler();
    Check(completed == 0 && board.startup_overlay_ != nullptr, "splash ended before 3 seconds");
    lv_tick_inc(1); lv_timer_handler();
    Check(completed == 1 && board.startup_overlay_ == nullptr, "splash did not finish at 3 seconds");
    Check(!board.startup_qr_image_, "splash pixels retained after finish");
    board.SetDiagnostic(DiagnosticChannel::Usb, "");
    board.SetDiagnostic(DiagnosticChannel::Qr, "");
    board.RefreshDiagnostics();
    Check(lv_obj_has_flag(board.diagnostic_label_, LV_OBJ_FLAG_HIDDEN), "resolved diagnostics stayed visible");
    board.SetDiagnosticStatus(DiagnosticChannel::Usb, "USB IMEI OK");
    board.SetDiagnosticStatus(DiagnosticChannel::Qr, "QR SAVED TF");
    board.SetDiagnosticStatus(DiagnosticChannel::Ble, "BLE D9:62:62:00:00:0D -83");
    board.SetDiagnosticStatus(DiagnosticChannel::OnlineReport, "KJ TX #1 BLE=1");
    board.RefreshDiagnostics();
    Check(lv_obj_has_flag(board.diagnostic_label_, LV_OBJ_FLAG_HIDDEN), "ordinary status covered normal pages");
    board.ToggleDiagnosticPage();
    lv_obj_update_layout(board.diagnostic_label_);
    Check(board.IsDiagnosticPageOpen(), "diagnostic page did not open");
    Check(std::strcmp(lv_label_get_text(board.diagnostic_label_),
          "USB IMEI OK\nNET WAIT\nQR SAVED TF\nWAV WAIT\nBLE D9:62:62:00:00:0D -83\n"
          "BIZ MQTT WAIT\nPOS MQTT WAIT\nKJ TX #1 BLE=1") == 0, "diagnostic page missing status");
    Check(lv_obj_get_y(board.diagnostic_label_) >= 0 &&
          lv_obj_get_y(board.diagnostic_label_) + lv_obj_get_height(board.diagnostic_label_) <= 135,
          "eight diagnostic lines do not fit the LCD");
    board.ToggleDiagnosticPage();
    Check(lv_obj_has_flag(board.diagnostic_label_, LV_OBJ_FLAG_HIDDEN), "diagnostic page did not close");
    Check(std::strcmp(lv_label_get_text(board.status_label_), "Initializing") == 0, "initialization status missing");
    board.SetStartupQrImage(std::make_unique<LvglImage>(300), "864479086534998");
    lv_tick_inc(5000); lv_timer_handler();
    Check(completed == 1 && !board.startup_qr_image_, "late QR reopens completed splash");
    std::cout << "PASS real LVGL: splash protects startup screen and finishes exactly once after 3000 ms\n";
    lv_obj_clean(lv_screen_active());
    TestDisplay delayed;
    delayed.BeginStartupPresentation([&]() { ++completed; });
    lv_tick_inc(10000); lv_timer_handler();
    delayed.ShowStartupInitializing();
    Check(lv_obj_has_flag(delayed.startup_overlay_, LV_OBJ_FLAG_HIDDEN), "network init should hide QR placeholder");
    delayed.SetStartupQrImage(std::make_unique<LvglImage>(300), "864479086534998");
    Check(!lv_obj_has_flag(delayed.startup_overlay_, LV_OBJ_FLAG_HIDDEN), "downloaded QR not presented");
    lv_tick_inc(2999); lv_timer_handler();
    Check(completed == 1, "download time counted toward QR display duration");
    lv_tick_inc(1); lv_timer_handler();
    Check(completed == 2, "delayed QR did not display for 3 seconds");
    std::cout << "PASS real LVGL: late download gets full 3-second presentation\n";
    lv_obj_clean(lv_screen_active());
    TestDisplay offline;
    offline.BeginStartupPresentation([&]() { ++completed; });
    lv_tick_inc(60000); lv_timer_handler();
    Check(completed == 2 && !offline.startup_finished_, "power-on timeout skipped QR presentation");
    offline.SetStartupQrImage(std::make_unique<LvglImage>(300), "864479086534998");
    lv_tick_inc(2999); lv_timer_handler();
    Check(completed == 2, "late first-boot download lost its display duration");
    lv_tick_inc(1); lv_timer_handler();
    Check(completed == 3, "late first-boot QR never completed");
    std::cout << "PASS real LVGL: download after 60 seconds still presents QR exactly once\n";
    // LVGL objects must be destroyed before the owned descriptor/pixels.
    lv_obj_clean(lv_screen_active());
    lv_display_delete(display);
    lv_deinit();
}
