#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "display/lvgl_display/lvgl_image.h"

#include <cstring>
#include <algorithm>
#include <cctype>
#include <new>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_app_desc.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"

namespace {
constexpr const char* kSdCardMountPoint = "/sdcard";
constexpr int64_t kRoleSwitchResponseTimeoutUs = 15LL * 1000 * 1000;
constexpr size_t kMaxSdAudioQueueSize = 32;
#ifdef CONFIG_BOARD_TYPE_JIUCHUAN
constexpr int kInitialVersionRetryDelaySeconds = 3;
constexpr int kMaximumVersionRetryDelaySeconds = 10;
constexpr int kDeferredOtaInitialDelaySeconds = 30;
constexpr int kDeferredOtaMaxAttempts = 6;
constexpr int kCustomerMqttPriorityWaitSeconds = 20;
#endif

enum class CachedProtocolType {
    None,
    Mqtt,
    Websocket,
};

CachedProtocolType GetCachedProtocolType() {
    Settings protocol_settings("protocol", false);
    const std::string preferred_type =
        protocol_settings.GetString("type");
    Settings websocket_settings("websocket", false);
    Settings mqtt_settings("mqtt", false);
    const bool has_websocket =
        !websocket_settings.GetString("url").empty();
    const bool has_mqtt = !mqtt_settings.GetString("endpoint").empty();

    if (preferred_type == "websocket" && has_websocket) {
        return CachedProtocolType::Websocket;
    }
    if (preferred_type == "mqtt" && has_mqtt) {
        return CachedProtocolType::Mqtt;
    }
    // Older firmware did not save the selected protocol type. The Jiuchuan
    // deployment uses WebSocket when both legacy namespaces are present.
    if (has_websocket) {
        return CachedProtocolType::Websocket;
    }
    return has_mqtt ? CachedProtocolType::Mqtt : CachedProtocolType::None;
}

bool NormalizeSdAudioFilename(const std::string& input, std::string& output) {
    if (input.empty() || input.size() > 192) {
        return false;
    }

    output = input;
    std::replace(output.begin(), output.end(), '\\', '/');
    while (output.rfind("./", 0) == 0) {
        output.erase(0, 2);
    }
    if (output.empty() || output.front() == '/' || output.find(':') != std::string::npos) {
        return false;
    }

    size_t segment_start = 0;
    while (segment_start <= output.size()) {
        const size_t segment_end = output.find('/', segment_start);
        const std::string segment = output.substr(
            segment_start,
            segment_end == std::string::npos ? std::string::npos : segment_end - segment_start);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        if (segment_end == std::string::npos) {
            break;
        }
        segment_start = segment_end + 1;
    }

    std::string lower = output;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const auto has_suffix = [&lower](const char* suffix) {
        const size_t suffix_size = std::strlen(suffix);
        return lower.size() >= suffix_size &&
               lower.compare(lower.size() - suffix_size, suffix_size, suffix) == 0;
    };
    return has_suffix(".wav") || has_suffix(".ogg") || has_suffix(".mp3");
}

bool IsSafeSdPathComponent(const std::string& value) {
    return !value.empty() && value.size() <= 96 &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '_' || ch == '-';
        });
}
}  // namespace


Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    if ((state == kDeviceStateListening || state == kDeviceStateSpeaking) &&
        Board::GetInstance().RequiresUserBinding() &&
        (!protocol_ || !protocol_->IsAudioChannelOpened())) {
        return false;
    }
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();

    // Keep protocol identity unchanged; only customize the Jiuchuan startup label.
#if defined(CONFIG_BOARD_TYPE_JIUCHUAN)
    display->SetChatMessage("system", "Vanex S3");
#else
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());
#endif

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

#ifndef CONFIG_BOARD_TYPE_JIUCHUAN
    PlaySdAudioFile("SafetyReminder.mp3");
#endif

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Confirm a newly booted OTA image after local hardware and core services
    // are healthy. Network availability must not control rollback validation.
    Ota startup_ota;
    startup_ota.MarkCurrentVersionValid();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data == "ML307 USB" || data == "USB RNDIS") {
                    // USB link and DHCP events can arrive out of order, including
                    // after the network is already ready. Keep transport-level
                    // events from regressing the application status display.
                } else if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                if (data == "ML307 USB" || data == "USB RNDIS") {
                    // The transport can report link-up before DHCP and can
                    // also deliver a delayed event after DHCP. The actual
                    // ready state is handled by HandleNetworkConnectedEvent;
                    // do not overwrite the current screen here.
                } else {
                    std::string msg = Lang::Strings::CONNECTED_TO;
                    msg += data;
                    display->ShowNotification(msg.c_str(), 30000);
                }
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                display->SetChatMessage("system", Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            if (Board::GetInstance().RequiresUserBinding() && protocol_ &&
                !protocol_->IsAudioChannelOpened()) {
                audio_service_.EnableVoiceProcessing(false);
                protocol_->CloseAudioChannel();
                if (!sd_audio_playing_.load()) audio_service_.ResetDecoder();
            }
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && protocol_->IsAudioChannelOpened() &&
                    !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
#ifdef CONFIG_BOARD_TYPE_JIUCHUAN
            TryStartTfStartAudio();
#endif
            if (role_switch_response_pending_ && role_switch_request_time_us_ > 0 &&
                esp_timer_get_time() - role_switch_request_time_us_ >
                    kRoleSwitchResponseTimeoutUs) {
                ESP_LOGW(TAG, "Role switch response timed out; clearing pending state");
                role_switch_response_pending_ = false;
                role_switch_request_time_us_ = 0;
                role_switch_restore_pending_ = false;
                role_switch_play_introduction_ = true;
                ResetRoleSwitchScan();
                CompleteRoleSwitch(false);
                if (GetDeviceState() == kDeviceStateConnecting) {
                    SetDeviceState(kDeviceStateIdle);
                }
            }
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
        FinishTtsWhenIdle();
        UpdateGuideDisplay();
    }
}

void Application::FinishTtsWhenIdle() {
    if (!tts_stop_pending_) return;
    if (GetDeviceState() != kDeviceStateSpeaking || aborted_) {
        tts_stop_pending_ = false;
        return;
    }
    // The server's stop packet can precede the final speaker samples.
    // Keep the narration page and mic state until those samples have drained.
    if (!audio_service_.IsIdle()) return;
    tts_stop_pending_ = false;
    const bool return_idle = return_idle_after_current_tts_ ||
        listening_mode_ == kListeningModeManualStop;
    return_idle_after_current_tts_ = false;
    SetDeviceState(return_idle ? kDeviceStateIdle : kDeviceStateListening);
}

void Application::UpdateGuideDisplay() {
#ifdef CONFIG_BOARD_TYPE_JIUCHUAN
    auto& board = Board::GetInstance();
    GuideInput input;
    input.state = GetDeviceState();
    input.startup_pending = board.IsStartupPresentationPending();
    input.local_playback = guide_local_playback_active_;
    input.playback = active_guide_playback_;
    input.role_name = GetCurrentRoleName();
    input.speech_text = guide_speech_text_;
    input.clock = GuideClock(std::time(nullptr));
    const auto binding = board.GetUserBinding();
    if (!board.RequiresUserBinding()) input.binding = GuideBinding::NotRequired;
    else if (!binding.IsBound()) input.binding = GuideBinding::Unbound;
    else if (binding.state == BindingState::Ready) input.binding = GuideBinding::Ready;
    else if (binding.state == BindingState::Error) input.binding = GuideBinding::Error;
    else input.binding = GuideBinding::Pending;
    board.GetDisplay()->SetGuideView(ResolveGuideView(input));
#endif
}

void Application::HandleNetworkConnectedEvent() {
    if (Board::GetInstance().IsStartupPresentationPending()) {
        startup_network_connected_pending_ = true;
        ESP_LOGI(TAG, "Network ready; deferring business initialization until startup screen ends");
        return;
    }
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // A delayed USB link event must not leave the UI at "initializing" after
    // the device has already returned to its normal idle state.
    if (state == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    startup_network_connected_pending_ = false;
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        if (protocol_) protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    if (ota_->HasMqttConfig() || ota_->HasWebsocketConfig()) {
        ESP_LOGI(TAG, "Activation done; server connection configuration received");
    } else if (GetCachedProtocolType() != CachedProtocolType::None) {
        ESP_LOGW(TAG, "Startup ready using cached connection configuration; server check pending");
    } else {
        ESP_LOGW(TAG, "Server configuration unavailable; local functions ready, provisioning pending");
    }

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
#ifndef CONFIG_BOARD_TYPE_JIUCHUAN
    std::string current_version = ota_->GetCurrentVersion();
    if (current_version.empty()) {
        current_version = esp_app_get_description()->version;
    }
    std::string message = std::string(Lang::Strings::VERSION) + current_version;
    display->ShowNotification(message.c_str());
#endif
    display->SetChatMessage("system", "");

#ifdef CONFIG_BOARD_TYPE_JIUCHUAN
    // TF initialization notifies us only after the file has been validated.
    TryStartTfStartAudio();
#else
    // Do not mix the OTA-ready sound into the initial safety announcement.
    if (!sd_audio_playing_.load() && !sd_audio_pending_.load()) {
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    } else {
        ESP_LOGI(TAG,
                 "Skipping activation success sound while local SD audio is playing");
    }
#endif

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

#ifdef CONFIG_BOARD_TYPE_JIUCHUAN
    if (deferred_ota_check_needed_) {
        ScheduleDeferredOtaCheck();
    }
#endif

    VerifyUserBinding();
    // The group response can now arrive before OTA/protocol initialization.
    // Retry the default role selection here if the safety reminder already
    // finished while protocol_ was still unavailable.
    if (initial_group_announcement_completed_ &&
        GetCurrentRoleId().empty() && !role_switch_response_pending_) {
        ESP_LOGI(TAG,
                 "Activation completed after safety reminder; requesting default role now");
        SwitchRoleVoiceBinding();
    }
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

#ifdef CONFIG_BOARD_TYPE_JIUCHUAN
    const bool has_cached_protocol =
        GetCachedProtocolType() != CachedProtocolType::None;
    if (has_cached_protocol) {
        // Cached protocol settings are sufficient to start the device. A
        // foreground OTA request here used to delay both protocol startup and
        // the customer's MQTT workflow, especially when RNDIS was still warm.
        deferred_ota_check_needed_ = true;
        ESP_LOGI(TAG,
                 "Cached protocol found; moving OTA version check after activation");
    } else {
        // A fresh device still needs the foreground check for activation code
        // and server configuration. Give customer MQTT first use of the new
        // data path so its online report and group response are not delayed by
        // OTA HTTP traffic.
        ESP_LOGI(TAG,
                 "Waiting up to %d seconds for the first customer MQTT online report before OTA",
                 kCustomerMqttPriorityWaitSeconds);
        const EventBits_t mqtt_bits = xEventGroupWaitBits(
            event_group_, MAIN_EVENT_CUSTOMER_MQTT_ONLINE,
            pdFALSE, pdTRUE,
            pdMS_TO_TICKS(kCustomerMqttPriorityWaitSeconds * 1000));
        ESP_LOGI(TAG, "Customer MQTT priority wait finished: online=%d",
                 (mqtt_bits & MAIN_EVENT_CUSTOMER_MQTT_ONLINE) != 0);
        CheckNewVersion();
    }
#else
    // Check for new firmware version
    CheckNewVersion();
#endif

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            }).detach();
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
#ifdef CONFIG_BOARD_TYPE_JIUCHUAN
    int retry_delay = kInitialVersionRetryDelaySeconds;
    const CachedProtocolType cached_protocol = GetCachedProtocolType();
    const bool has_cached_protocol =
        cached_protocol != CachedProtocolType::None;
    ESP_LOGI(TAG, "OTA startup policy: cached_protocol=%s",
             cached_protocol == CachedProtocolType::Websocket ? "websocket" :
             cached_protocol == CachedProtocolType::Mqtt ? "mqtt" : "none");
#else
    int retry_delay = 10; // Initial retry delay in seconds
#endif

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
#ifndef CONFIG_BOARD_TYPE_JIUCHUAN
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);
#endif

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
#ifdef CONFIG_BOARD_TYPE_JIUCHUAN
            if (has_cached_protocol) {
                deferred_ota_check_needed_ = true;
                ESP_LOGW(TAG,
                         "Fast OTA check failed (code=0x%x); continuing with cached %s config",
                         static_cast<unsigned>(err),
                         cached_protocol == CachedProtocolType::Websocket
                             ? "websocket" : "mqtt");
                return;
            }
#endif
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
#ifdef CONFIG_BOARD_TYPE_JIUCHUAN
                deferred_ota_check_needed_ = true;
#endif
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
#ifdef CONFIG_BOARD_TYPE_JIUCHUAN
            // A fresh device has no cached server address, so it must keep
            // provisioning in the foreground. Keep retry details in the
            // serial log without exposing internal URLs on the device screen.
            ESP_LOGW(TAG, "Version check retry detail: %s", buffer);
            display->SetChatMessage("system", "");
#else
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);
#endif

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
#ifdef CONFIG_BOARD_TYPE_JIUCHUAN
            retry_delay = std::min(retry_delay + 2,
                                   kMaximumVersionRetryDelaySeconds);
#else
            retry_delay *= 2; // Double the retry delay
#endif
            continue;
        }
        retry_count = 0;
#ifdef CONFIG_BOARD_TYPE_JIUCHUAN
        retry_delay = kInitialVersionRetryDelaySeconds;
        deferred_ota_check_needed_ = false;
#else
        retry_delay = 10; // Reset retry delay
#endif

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (board.RequiresUserBinding()) {
        // MQTT/UDP has no verified user-context handshake. Never fall back to it.
        ESP_LOGI(TAG, "Verified user binding requires WebSocket audio");
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else if (ota_->HasMqttConfig()) {
        ESP_LOGI(TAG, "Using MQTT config returned by OTA");
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        ESP_LOGI(TAG, "Using WebSocket config returned by OTA");
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        const CachedProtocolType cached_protocol = GetCachedProtocolType();
        if (cached_protocol == CachedProtocolType::Websocket) {
            ESP_LOGW(TAG, "OTA config unavailable; using cached WebSocket config");
            protocol_ = std::make_unique<WebsocketProtocol>();
        } else {
            ESP_LOGW(TAG, "OTA config unavailable; using %s MQTT config",
                     cached_protocol == CachedProtocolType::Mqtt
                         ? "cached" : "default");
            protocol_ = std::make_unique<MqttProtocol>();
        }
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
        const std::string current_role_id = GetCurrentRoleId();
        if (!current_role_id.empty() && !role_switch_response_pending_) {
            ESP_LOGI(TAG,
                     "Protocol reconnected; restoring server role binding: %s",
                     current_role_id.c_str());
            SelectRoleVoiceBinding(current_role_id, {}, false);
        }
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        ++audio_channel_epoch_;
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        const uint32_t epoch = ++audio_channel_epoch_;
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this, epoch]() {
            if (epoch != audio_channel_epoch_.load()) return;
            audio_service_.EnableVoiceProcessing(false);
            if (!sd_audio_playing_.load()) audio_service_.ResetDecoder();
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming JSON message has no string type");
            return;
        }
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) {
                ESP_LOGW(TAG, "Incoming TTS message has no string state");
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                ScheduleAudioChannelTask([this]() {
                    const DeviceState previous_state = GetDeviceState();
                    if (!tts_stop_pending_)
                        return_idle_after_current_tts_ = previous_state == kDeviceStateIdle;
                    tts_stop_pending_ = false;
                    aborted_ = false;
                    guide_speech_text_.clear();
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                ScheduleAudioChannelTask([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        tts_stop_pending_ = true;
                        FinishTtsWhenIdle();
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    ScheduleAudioChannelTask([this, display, message = std::string(text->valuestring)]() {
                        guide_speech_text_ = message;
                        display->SetPreviewImage(nullptr);
                        display->SetContentFooter("");
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                RecordConversation();
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                ScheduleAudioChannelTask([this, display, message = std::string(text->valuestring)]() {
                    guide_speech_text_ = message;
                    display->SetPreviewImage(nullptr);
                    display->SetContentFooter("");
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "role_switch") == 0) {
            auto state = cJSON_GetObjectItemCaseSensitive(root, "state");
            auto role_id = cJSON_GetObjectItemCaseSensitive(root, "roleId");
            if (!cJSON_IsString(role_id)) {
                role_id = cJSON_GetObjectItemCaseSensitive(root, "role_id");
            }
            auto role_name = cJSON_GetObjectItemCaseSensitive(root, "roleName");
            auto binding_count = cJSON_GetObjectItemCaseSensitive(root, "bindingCount");
            if (cJSON_IsNumber(binding_count) && binding_count->valuedouble > 0) {
                role_switch_binding_count_ =
                    static_cast<size_t>(binding_count->valuedouble);
            }
            const bool success = cJSON_IsString(state) && state->valuestring &&
                std::strcmp(state->valuestring, "success") == 0 &&
                cJSON_IsString(role_id) && role_id->valuestring &&
                role_id->valuestring[0] != '\0';
            const std::string response_role_id = success ? role_id->valuestring : "";
            const std::string response_role_name =
                success && cJSON_IsString(role_name) && role_name->valuestring
                    ? role_name->valuestring
                    : "";

            if (role_switch_restore_pending_) {
                role_switch_response_pending_ = false;
                role_switch_request_time_us_ = 0;
                role_switch_restore_pending_ = false;
                role_switch_play_introduction_ = true;
                if (success) {
                    SetCurrentRole(response_role_id, response_role_name);
                    ESP_LOGI(TAG,
                             "[ROLE_SWITCH][RESTORED] role=%s after candidate scan",
                             response_role_id.c_str());
                } else {
                    ESP_LOGW(TAG, "[ROLE_SWITCH][RESTORE_FAILED]");
                }
                ResetRoleSwitchScan();
                CompleteRoleSwitch(success);
                if (GetDeviceState() == kDeviceStateConnecting) {
                    SetDeviceState(kDeviceStateIdle);
                }
                return;
            }

            if (success && role_switch_scan_active_ &&
                ShouldValidateRoleIntroduction() &&
                !RoleIntroductionFileExists(response_role_id)) {
                if (role_switch_first_role_id_.empty()) {
                    role_switch_first_role_id_ = response_role_id;
                    role_switch_first_role_name_ = response_role_name;
                }
                const bool repeated = std::find(
                    role_switch_seen_role_ids_.begin(),
                    role_switch_seen_role_ids_.end(),
                    response_role_id) != role_switch_seen_role_ids_.end();
                if (!repeated) {
                    role_switch_seen_role_ids_.push_back(response_role_id);
                }
                const bool exhausted = repeated ||
                    (role_switch_binding_count_ > 0 &&
                     role_switch_seen_role_ids_.size() >= role_switch_binding_count_) ||
                    role_switch_seen_role_ids_.size() >= 32;

                ESP_LOGW(TAG,
                         "[ROLE_SWITCH][SKIP_MISSING_SD] role=%s checked=%u total=%u",
                         response_role_id.c_str(),
                         static_cast<unsigned>(role_switch_seen_role_ids_.size()),
                         static_cast<unsigned>(role_switch_binding_count_));
                if (!exhausted) {
                    SendNextRoleSwitchCandidate();
                    return;
                }

                const std::string fallback_role_id =
                    !role_switch_origin_role_id_.empty()
                        ? role_switch_origin_role_id_
                        : role_switch_first_role_id_;
                ESP_LOGW(TAG,
                         "[ROLE_SWITCH][NO_PLAYABLE_ROLE] checked=%u fallback=%s",
                         static_cast<unsigned>(role_switch_seen_role_ids_.size()),
                         fallback_role_id.empty() ? "<none>" : fallback_role_id.c_str());
                if (!fallback_role_id.empty() &&
                    fallback_role_id != response_role_id) {
                    SendRoleSwitchRestore(fallback_role_id);
                    return;
                }

                role_switch_response_pending_ = false;
                role_switch_request_time_us_ = 0;
                if (role_switch_origin_role_id_.empty()) {
                    // First boot has no previous role. Keep one configured role
                    // active, but never queue a file which is known to be absent.
                    SetCurrentRole(response_role_id, response_role_name);
                }
                role_switch_play_introduction_ = true;
                ResetRoleSwitchScan();
                CompleteRoleSwitch(false);
                if (GetDeviceState() == kDeviceStateConnecting) {
                    SetDeviceState(kDeviceStateIdle);
                }
                return;
            }

            if (success) {
                SetCurrentRole(response_role_id, response_role_name);
            } else {
                ESP_LOGW(TAG, "Role switch failed or result has no role ID");
            }
            const bool play_introduction =
                success && role_switch_play_introduction_;
            role_switch_response_pending_ = false;
            role_switch_request_time_us_ = 0;
            role_switch_play_introduction_ = true;
            ResetRoleSwitchScan();
            CompleteRoleSwitch(success);
            if (play_introduction) {
                PlayCurrentRoleIntroduction();
            } else if (GetDeviceState() == kDeviceStateConnecting) {
                SetDeviceState(kDeviceStateIdle);
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                ScheduleAudioChannelTask([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            char* message_json = cJSON_PrintUnformatted(root);
            ESP_LOGI(TAG, "Received custom message: %s", message_json ? message_json : "{}");
            cJSON_free(message_json);
            if (cJSON_IsObject(payload)) {
                auto command = cJSON_GetObjectItem(payload, "command");
                if (cJSON_IsString(command) && strcmp(command->valuestring, "play_sd_audio") == 0) {
                    auto file = cJSON_GetObjectItem(payload, "file");
                    if (cJSON_IsString(file)) {
                        PlaySdAudioFile(file->valuestring);
                    } else {
                        ESP_LOGW(TAG, "play_sd_audio requires a string file field");
                    }
                } else {
                    ESP_LOGW(TAG, "Unknown custom command");
                }
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();
}

#ifdef CONFIG_BOARD_TYPE_JIUCHUAN
void Application::ScheduleDeferredOtaCheck() {
    if (deferred_ota_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "Deferred OTA check is already scheduled");
        return;
    }

    ESP_LOGI(TAG, "Deferred OTA check scheduled in %d seconds",
             kDeferredOtaInitialDelaySeconds);
    BaseType_t result = xTaskCreate([](void* arg) {
        auto* app = static_cast<Application*>(arg);
        vTaskDelay(pdMS_TO_TICKS(kDeferredOtaInitialDelaySeconds * 1000));

        int retry_delay_seconds = 30;
        for (int attempt = 1; attempt <= kDeferredOtaMaxAttempts; ++attempt) {
            while (app->GetDeviceState() != kDeviceStateIdle) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }

            ESP_LOGI(TAG, "Deferred OTA check attempt %d/%d",
                     attempt, kDeferredOtaMaxAttempts);
            Ota deferred_ota;
            esp_err_t err = deferred_ota.CheckVersion();
            if (err == ESP_OK &&
                (deferred_ota.HasActivationCode() || deferred_ota.HasActivationChallenge())) {
                ESP_LOGW(TAG, "Deferred configuration requires device activation; provisioning remains pending");
                if (deferred_ota.HasActivationCode()) {
                    const auto code = deferred_ota.GetActivationCode();
                    const auto message = deferred_ota.GetActivationMessage();
                    app->Schedule([app, code, message]() { app->ShowActivationCode(code, message); });
                }
                deferred_ota.Activate();
                // Re-fetch config next time; an activation response alone is
                // not proof that the device now has usable voice credentials.
                err = ESP_ERR_INVALID_STATE;
            }
            if (err == ESP_OK) {
                deferred_ota.MarkCurrentVersionValid();
                app->deferred_ota_check_needed_ = false;
                const bool has_time = deferred_ota.HasServerTime();
                app->Schedule([app, has_time]() {
                    if (has_time) app->has_server_time_ = true;
                    app->VerifyUserBinding();
                });
                if (deferred_ota.HasNewVersion()) {
                    const std::string url = deferred_ota.GetFirmwareUrl();
                    const std::string version = deferred_ota.GetFirmwareVersion();
                    while (app->GetDeviceState() != kDeviceStateIdle) {
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    }
                    ESP_LOGI(TAG,
                             "Deferred OTA found version %s; starting upgrade while idle",
                             version.c_str());
                    app->UpgradeFirmware(url, version);
                } else {
                    ESP_LOGI(TAG, "Deferred OTA check succeeded; firmware is current");
                }
                break;
            }

            ESP_LOGW(TAG,
                     "Deferred OTA check failed, code=0x%x, retry in %d seconds",
                     static_cast<unsigned>(err), retry_delay_seconds);
            if (attempt < kDeferredOtaMaxAttempts) {
                vTaskDelay(pdMS_TO_TICKS(retry_delay_seconds * 1000));
                retry_delay_seconds = std::min(retry_delay_seconds * 2, 300);
            }
        }

        app->deferred_ota_task_handle_ = nullptr;
        vTaskDelete(nullptr);
    }, "deferred_ota", 4096 * 2, this, 1, &deferred_ota_task_handle_);

    if (result != pdPASS) {
        deferred_ota_task_handle_ = nullptr;
        ESP_LOGE(TAG, "Failed to create deferred OTA task");
    }
}
#else
void Application::ScheduleDeferredOtaCheck() {}
#endif

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::SwitchRoleVoiceBinding() {
    Schedule([this]() {
        if (!CanStartPersonalChat()) return;
        if (role_switch_response_pending_) {
            ESP_LOGW(TAG, "Role switch ignored: another role request is pending");
            return;
        }
        if (!protocol_) {
            ESP_LOGE(TAG, "Cannot switch role: protocol is not initialized");
            return;
        }

        if (!initial_group_announcement_requested_ ||
            initial_group_announcement_completed_) {
            CancelSdAudioPlaybackOnMainTask();
        }

        if (GetDeviceState() == kDeviceStateSpeaking) {
            AbortSpeaking(kAbortReasonNone);
        }

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                ESP_LOGE(TAG, "Cannot switch role: failed to open audio channel");
                SetDeviceState(kDeviceStateIdle);
                return;
            }
            SetDeviceState(kDeviceStateIdle);
        }

        ResetRoleSwitchScan();
        role_switch_scan_active_ = true;
        role_switch_play_introduction_ = true;
        role_switch_origin_role_id_ = GetCurrentRoleId();
        role_switch_origin_role_name_ = GetCurrentRoleName();
        requested_role_id_.clear();
        ESP_LOGI(TAG, "Request next role/voice binding");
        SendNextRoleSwitchCandidate();
    });
}

void Application::SelectRoleVoiceBinding(
    const std::string& role_id, std::function<void(bool)> completion,
    bool play_introduction) {
    if (role_id.empty()) {
        if (completion) {
            completion(false);
        }
        return;
    }

    Schedule([this, role_id, play_introduction,
              completion = std::move(completion)]() mutable {
        if (!CanStartPersonalChat()) {
            if (completion) completion(false);
            return;
        }
        if (role_switch_response_pending_) {
            ESP_LOGW(TAG, "Role selection ignored: another role request is pending");
            if (completion) {
                completion(false);
            }
            return;
        }
        if (!protocol_) {
            ESP_LOGE(TAG, "Cannot select role: protocol is not initialized");
            if (completion) {
                completion(false);
            }
            return;
        }

        if (play_introduction && ShouldValidateRoleIntroduction() &&
            !RoleIntroductionFileExists(role_id)) {
            ESP_LOGW(TAG,
                     "[ROLE_SWITCH][SELECT_REJECTED_MISSING_SD] role=%s",
                     role_id.c_str());
            if (completion) {
                completion(false);
            }
            return;
        }

        if (!initial_group_announcement_requested_ ||
            initial_group_announcement_completed_) {
            CancelSdAudioPlaybackOnMainTask();
        }

        if (GetDeviceState() == kDeviceStateSpeaking) {
            AbortSpeaking(kAbortReasonNone);
        }

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                ESP_LOGE(TAG, "Cannot select role: failed to open audio channel");
                SetDeviceState(kDeviceStateIdle);
                if (completion) {
                    completion(false);
                }
                return;
            }
            SetDeviceState(kDeviceStateIdle);
        }

        requested_role_id_ = role_id;
        role_switch_play_introduction_ = play_introduction;
        role_switch_completion_ = std::move(completion);
        role_switch_response_pending_ = true;
        role_switch_request_time_us_ = esp_timer_get_time();
        ESP_LOGI(TAG, "Request selected role/voice binding: %s", role_id.c_str());
        protocol_->SendRoleSwitchSelect(role_id);
    });
}

void Application::HandleContentGroupId(const std::string& group_id) {
    if (!IsSafeSdPathComponent(group_id)) {
        ESP_LOGW(TAG, "Rejected invalid content group ID: %s", group_id.c_str());
        return;
    }
    Schedule([this, group_id]() {
        if (content_group_id_ != group_id) {
            content_group_id_ = group_id;
            ESP_LOGI(TAG, "Content group updated: %s", group_id.c_str());
        }
        if (!initial_group_announcement_requested_) {
            initial_group_announcement_requested_ = true;
            ESP_LOGI(TAG, "First group ID received; initial announcement requested");
        }
        TryStartInitialGroupAnnouncement();
        if (GetCurrentRoleId().empty() && !role_switch_response_pending_) {
            ESP_LOGI(TAG,
                     "No default role is selected; requesting the first configured role");
            SwitchRoleVoiceBinding();
        }
    });
}

void Application::NotifyCustomerMqttOnline() {
    if (event_group_ != nullptr) {
        ESP_LOGI(TAG, "Customer MQTT online report completed; background OTA may continue");
        xEventGroupSetBits(event_group_, MAIN_EVENT_CUSTOMER_MQTT_ONLINE);
    }
}

void Application::NotifySdContentSynchronized(const std::string& group_id) {
    if (!IsSafeSdPathComponent(group_id)) {
        return;
    }
    Schedule([this, group_id]() {
        if (initial_group_announcement_requested_ &&
            !initial_group_announcement_started_ &&
            content_group_id_ == group_id) {
            ESP_LOGI(TAG, "SD content synchronized; retrying initial announcement");
            TryStartInitialGroupAnnouncement();
        }
    });
}

void Application::NotifyStartupPresentationDone() {
    Schedule([this]() {
        if (startup_network_connected_pending_) {
            startup_network_connected_pending_ = false;
            HandleNetworkConnectedEvent();
        }
    });
}

void Application::NotifyTfStartAudioReady() {
    Schedule([this]() {
        if (!tf_start_audio_ready_) {
            ESP_LOGI(TAG, "[BOOT_AUDIO][READY] /sdcard/start.wav");
        }
        tf_start_audio_ready_ = true;
        TryStartTfStartAudio();
    });
}

void Application::TryStartTfStartAudio() {
    if (!tf_start_audio_ready_ || tf_start_audio_started_) {
        return;
    }
    const auto state = GetDeviceState();
    if ((state != kDeviceStateStarting && state != kDeviceStateIdle) ||
        sd_audio_playing_.load() || sd_audio_pending_.load() ||
        !sd_audio_queue_.empty() || !audio_service_.IsIdle()) {
        return;
    }

    // Called only on the main task. Enqueue immediately so a later user action
    // can cancel this item instead of being interrupted by delayed scheduling.
    tf_start_audio_started_ = true;
    SdAudioQueueItem item;
    item.filename = "start.wav";
    item.first = true;
    item.last = true;
    item.sequence = std::make_shared<SdAudioSequenceState>();
    item.sequence->completion = [](bool success) {
        if (success) {
            ESP_LOGI(TAG, "[BOOT_AUDIO][DONE] /sdcard/start.wav");
        } else {
            ESP_LOGW(TAG, "[BOOT_AUDIO][STOPPED] failed or interrupted; no repeat this boot");
        }
    };
    sd_audio_queue_.push_back(std::move(item));
    sd_audio_pending_.store(true);
    ESP_LOGI(TAG, "[BOOT_AUDIO][QUEUED] /sdcard/start.wav; once per boot");
    StartNextSdAudioPlayback();
}

void Application::PlaySdAudioFile(const std::string& filename) {
    GuidePlayback presentation;
    const auto slash = filename.find_last_of('/');
    const auto name = filename.substr(slash == std::string::npos ? 0 : slash + 1);
    if (name == "SafetyReminder.mp3") presentation.kind = GuideAudioKind::Safety;
    else if (name == "start.wav") presentation.kind = GuideAudioKind::Startup;
    PlaySdAudioSequence({filename}, "", "", "", {}, presentation);
}

void Application::PlaySdAudioSequence(
    const std::vector<std::string>& filenames,
    const std::string& display_text,
    const std::string& image_filename,
    const std::string& footer,
    std::function<void(bool)> completion,
    const GuidePlayback& presentation) {
    std::vector<std::string> normalized_files;
    normalized_files.reserve(filenames.size());
    for (const auto& filename : filenames) {
        std::string normalized;
        if (!NormalizeSdAudioFilename(filename, normalized)) {
            ESP_LOGW(TAG, "Rejected unsafe or unsupported SD audio filename: %s",
                     filename.c_str());
            if (completion) {
                completion(false);
            }
            return;
        }
        normalized_files.push_back(std::move(normalized));
    }
    if (normalized_files.empty()) {
        if (completion) {
            completion(false);
        }
        return;
    }

    Schedule([this, normalized_files = std::move(normalized_files),
              display_text, image_filename, footer, presentation,
              completion = std::move(completion)]() mutable {
        if (sd_audio_queue_.size() + normalized_files.size() >
            kMaxSdAudioQueueSize) {
            ESP_LOGW(TAG, "SD audio queue has no room for %u files",
                     static_cast<unsigned>(normalized_files.size()));
            if (completion) {
                completion(false);
            }
            return;
        }

        auto sequence = std::make_shared<SdAudioSequenceState>();
        sequence->completion = std::move(completion);
        sequence->presentation = presentation;
        if (sequence->presentation.text.empty()) sequence->presentation.text = display_text;
        sd_audio_pending_.store(true);
        for (size_t index = 0; index < normalized_files.size(); ++index) {
            SdAudioQueueItem item;
            item.filename = std::move(normalized_files[index]);
            item.sequence = sequence;
            item.first = index == 0;
            item.last = index + 1 == normalized_files.size();
            if (item.first) {
                item.display_text = display_text;
                item.image_filename = image_filename;
                item.footer = footer;
            }
            sd_audio_queue_.push_back(std::move(item));
        }
        ESP_LOGI(TAG, "Queued SD audio sequence: files=%u queue_size=%u",
                 static_cast<unsigned>(normalized_files.size()),
                 static_cast<unsigned>(sd_audio_queue_.size()));
        StartNextSdAudioPlayback();
    });
}

void Application::InterruptCurrentPlayback() {
    Schedule([this]() {
        CancelSdAudioPlaybackOnMainTask();
    });
}

void Application::CancelSdAudioPlaybackOnMainTask() {
    guide_local_playback_active_ = false;
    for (auto& item : sd_audio_queue_) {
        if (item.last && item.sequence && item.sequence->completion) {
            item.sequence->completion(false);
            item.sequence->completion = {};
        }
    }
    sd_audio_queue_.clear();
    sd_audio_pending_.store(false);
    audio_service_.CancelFilePlayback();
    Board::GetInstance().GetDisplay()->SetPreviewImage(nullptr);
    Board::GetInstance().GetDisplay()->SetContentFooter("");
    if (GetDeviceState() == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetDeviceState(kDeviceStateIdle);
    }
    ESP_LOGI(TAG, "Current playback interrupted");
}

uint64_t Application::GetSdAudioPlaybackDurationSeconds() const {
    std::lock_guard<std::mutex> lock(statistics_mutex_);
    return sd_audio_playback_duration_ms_ / 1000;
}

uint32_t Application::GetConversationCount() const {
    std::lock_guard<std::mutex> lock(statistics_mutex_);
    return conversation_count_;
}

std::string Application::GetCurrentRoleId() const {
    std::lock_guard<std::mutex> lock(role_mutex_);
    return current_role_id_;
}

std::string Application::GetCurrentRoleName() const {
    std::lock_guard<std::mutex> lock(role_mutex_);
    return current_role_name_;
}

void Application::RecordSdAudioPlaybackDuration(uint64_t duration_ms) {
    std::lock_guard<std::mutex> lock(statistics_mutex_);
    sd_audio_playback_duration_ms_ += duration_ms;
}

void Application::RecordConversation() {
    std::lock_guard<std::mutex> lock(statistics_mutex_);
    ++conversation_count_;
}

void Application::SetCurrentRole(const std::string& role_id,
                                 const std::string& role_name) {
    {
        std::lock_guard<std::mutex> lock(role_mutex_);
        current_role_id_ = role_id;
        current_role_name_ = role_name;
    }
    ESP_LOGI(TAG, "Current role updated: id=%s name=%s",
             role_id.empty() ? "<none>" : role_id.c_str(),
             role_name.empty() ? "<none>" : role_name.c_str());
}

void Application::CompleteRoleSwitch(bool success) {
    if (!role_switch_completion_) {
        return;
    }
    if (success && !requested_role_id_.empty() &&
        GetCurrentRoleId() != requested_role_id_) {
        success = false;
    }
    auto completion = std::move(role_switch_completion_);
    role_switch_completion_ = {};
    requested_role_id_.clear();
    completion(success);
}

bool Application::ShouldValidateRoleIntroduction() const {
    return IsSafeSdPathComponent(content_group_id_) &&
        (initial_group_announcement_started_ ||
         initial_group_announcement_completed_);
}

bool Application::RoleIntroductionFileExists(
    const std::string& role_id, std::string* relative_path) const {
    if (!IsSafeSdPathComponent(content_group_id_) ||
        !IsSafeSdPathComponent(role_id)) {
        return false;
    }
    const std::string relative = content_group_id_ + "/" + role_id +
        "/effect/CharacterIntroduction.mp3";
    const std::string absolute =
        std::string(kSdCardMountPoint) + "/" + relative;
    struct stat file_info = {};
    const bool exists = stat(absolute.c_str(), &file_info) == 0 &&
        S_ISREG(file_info.st_mode);
    if (relative_path) {
        *relative_path = relative;
    }
    return exists;
}

void Application::ResetRoleSwitchScan() {
    role_switch_scan_active_ = false;
    role_switch_binding_count_ = 0;
    role_switch_origin_role_id_.clear();
    role_switch_origin_role_name_.clear();
    role_switch_first_role_id_.clear();
    role_switch_first_role_name_.clear();
    role_switch_seen_role_ids_.clear();
}

void Application::SendNextRoleSwitchCandidate() {
    role_switch_response_pending_ = true;
    role_switch_request_time_us_ = esp_timer_get_time();
    protocol_->SendRoleSwitchNext();
}

void Application::SendRoleSwitchRestore(const std::string& role_id) {
    role_switch_scan_active_ = false;
    role_switch_restore_pending_ = true;
    role_switch_play_introduction_ = false;
    requested_role_id_ = role_id;
    role_switch_response_pending_ = true;
    role_switch_request_time_us_ = esp_timer_get_time();
    ESP_LOGI(TAG, "[ROLE_SWITCH][RESTORE_REQUEST] role=%s", role_id.c_str());
    protocol_->SendRoleSwitchSelect(role_id);
}

void Application::TryStartInitialGroupAnnouncement() {
    if (!initial_group_announcement_requested_ ||
        initial_group_announcement_started_ ||
        initial_group_announcement_completed_ ||
        !IsSafeSdPathComponent(content_group_id_)) {
        return;
    }

    std::string relative_path =
        content_group_id_ + "/effect/SafetyReminder.mp3";
    std::string absolute_path =
        std::string(kSdCardMountPoint) + "/" + relative_path;
    struct stat file_info = {};
    if (stat(absolute_path.c_str(), &file_info) != 0 || !S_ISREG(file_info.st_mode)) {
        // Accept the original /groupID/SafetyReminder.mp3 layout as well.
        relative_path = content_group_id_ + "/SafetyReminder.mp3";
        absolute_path = std::string(kSdCardMountPoint) + "/" + relative_path;
    }
    if (stat(absolute_path.c_str(), &file_info) != 0 ||
        !S_ISREG(file_info.st_mode)) {
        ESP_LOGW(TAG,
                 "Initial safety reminder is not available yet: %s; waiting for SD sync",
                 absolute_path.c_str());
        return;
    }

    initial_group_announcement_started_ = true;
    ESP_LOGI(TAG, "Playing initial safety reminder: %s", absolute_path.c_str());

    // The safety reminder is mandatory startup content. Keep the screen awake
    // until playback completes so the inactivity timer cannot hide it.
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    if (auto* display = board.GetDisplay()) {
        display->SetPowerSaveMode(false);
    }
    if (auto* backlight = board.GetBacklight()) {
        backlight->RestoreBrightness();
    }

    PlaySdAudioSequence(
        {relative_path}, "安全提示", "", "",
        [this](bool success) {
            auto& board = Board::GetInstance();
            if (auto* display = board.GetDisplay()) {
                display->SetPowerSaveMode(false);
            }
            if (auto* backlight = board.GetBacklight()) {
                backlight->RestoreBrightness();
            }

            if (!success) {
                initial_group_announcement_started_.store(false);
                ESP_LOGW(TAG,
                         "Initial safety reminder did not finish; keeping the screen awake and waiting to retry");
                return;
            }
            initial_group_announcement_completed_.store(true);
            if (!GetCurrentRoleId().empty()) {
                PlayCurrentRoleIntroduction();
            } else if (role_switch_response_pending_) {
                ESP_LOGI(TAG,
                         "Safety reminder completed; waiting for default role response");
            } else {
                ESP_LOGI(TAG,
                         "No current role after safety reminder; selecting the first configured role");
                SwitchRoleVoiceBinding();
            }
        }, GuidePlayback{GuideAudioKind::Safety, "", "", ""});
}

void Application::PlayCurrentRoleIntroduction() {
    Schedule([this]() {
        if (initial_group_announcement_requested_ &&
            !initial_group_announcement_completed_) {
            ESP_LOGI(TAG,
                     "Role introduction deferred until the initial safety reminder finishes");
            return;
        }

        const std::string role_id = GetCurrentRoleId();
        const std::string role_name = GetCurrentRoleName();
        if (!IsSafeSdPathComponent(content_group_id_) ||
            !IsSafeSdPathComponent(role_id)) {
            ESP_LOGW(TAG,
                     "Cannot play role introduction: invalid group or role ID (group=%s role=%s)",
                     content_group_id_.c_str(), role_id.c_str());
            return;
        }

        const std::string relative_path = content_group_id_ + "/" + role_id +
            "/effect/CharacterIntroduction.mp3";
        if (!RoleIntroductionFileExists(role_id)) {
            ESP_LOGW(TAG,
                     "[ROLE_SWITCH][INTRO_MISSING] /sdcard/%s; returning to idle",
                     relative_path.c_str());
            if (GetDeviceState() == kDeviceStateConnecting ||
                GetDeviceState() == kDeviceStateSpeaking) {
                SetDeviceState(kDeviceStateIdle);
            }
            return;
        }
        ESP_LOGI(TAG, "Playing local role introduction: /sdcard/%s",
                 relative_path.c_str());
        PlaySdAudioSequence(
            {relative_path}, role_name.empty() ? "角色介绍" : role_name,
            "", "",
            [relative_path](bool success) {
                if (!success) {
                    ESP_LOGW(TAG, "Local role introduction failed: /sdcard/%s",
                             relative_path.c_str());
                }
            }, GuidePlayback{GuideAudioKind::Narration, role_name, "角色介绍", ""});
    });
}

std::unique_ptr<LvglImage> Application::LoadSdPreviewImage(
    const std::string& filename) const {
    if (filename.empty() || filename.front() == '/' ||
        filename.find(':') != std::string::npos ||
        filename.find("..") != std::string::npos) {
        return nullptr;
    }
    const std::string path = std::string(kSdCardMountPoint) + "/" + filename;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        ESP_LOGW(TAG, "SD preview image not found: %s", path.c_str());
        return nullptr;
    }
    const std::streamsize size = file.tellg();
    constexpr std::streamsize kMaxPreviewImageBytes = 4 * 1024 * 1024;
    if (size <= 0 || size > kMaxPreviewImageBytes) {
        ESP_LOGW(TAG, "Invalid SD preview image size: %lld",
                 static_cast<long long>(size));
        return nullptr;
    }
    file.seekg(0, std::ios::beg);
    void* data = heap_caps_malloc(static_cast<size_t>(size),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        data = heap_caps_malloc(static_cast<size_t>(size), MALLOC_CAP_8BIT);
    }
    if (!data || !file.read(static_cast<char*>(data), size)) {
        if (data) {
            heap_caps_free(data);
        }
        ESP_LOGW(TAG, "Failed to read SD preview image: %s", path.c_str());
        return nullptr;
    }
    try {
        return std::make_unique<LvglAllocatedImage>(data,
                                                    static_cast<size_t>(size));
    } catch (const std::exception& error) {
        heap_caps_free(data);
        ESP_LOGW(TAG, "Failed to decode SD preview image %s: %s",
                 path.c_str(), error.what());
        return nullptr;
    }
}

void Application::StartNextSdAudioPlayback() {
    if (sd_audio_playing_.load()) {
        return;
    }

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    while (!sd_audio_queue_.empty()) {
        SdAudioQueueItem item = std::move(sd_audio_queue_.front());
        sd_audio_queue_.pop_front();
        const std::string path = std::string(kSdCardMountPoint) + "/" + item.filename;

        struct stat file_info = {};
        if (stat(path.c_str(), &file_info) != 0 || !S_ISREG(file_info.st_mode)) {
            ESP_LOGW(TAG, "[SD_PLAY][MISSING] path=%s", path.c_str());
            if (item.sequence) {
                item.sequence->success = false;
                if (item.last && item.sequence->completion) {
                    item.sequence->completion(false);
                    item.sequence->completion = {};
                }
            }
            continue;
        }

        const DeviceState state = GetDeviceState();
        if (state == kDeviceStateSpeaking) {
            AbortSpeaking(kAbortReasonNone);
            SetDeviceState(kDeviceStateIdle);
        } else if (state == kDeviceStateListening) {
            if (protocol_) {
                protocol_->SendStopListening();
            }
            SetDeviceState(kDeviceStateIdle);
        }

        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        audio_service_.ResetDecoder();
        if (item.first) {
            active_guide_playback_ = item.sequence ? item.sequence->presentation : GuidePlayback{};
            if (item.filename == "start.wav") active_guide_playback_.kind = GuideAudioKind::Startup;
            guide_local_playback_active_ = active_guide_playback_.kind != GuideAudioKind::Startup;
            auto preview = LoadSdPreviewImage(item.image_filename);
            if (preview) {
                display->SetChatMessage("assistant", "");
                display->SetPreviewImage(std::move(preview));
            } else {
                display->SetPreviewImage(nullptr);
                display->SetChatMessage("assistant", item.display_text.c_str());
            }
            display->SetContentFooter(item.footer.c_str());
        }
        sd_audio_playing_.store(true);

        struct PlaybackTaskContext {
            Application* app;
            std::string path;
            SdAudioQueueItem item;
            uint32_t generation;
        };
        auto* context = new (std::nothrow) PlaybackTaskContext{
            this, path, std::move(item),
            audio_service_.GetFilePlaybackGeneration()};
        if (!context) {
            ESP_LOGE(TAG, "[SD_PLAY][ERROR] context allocation failed path=%s",
                     path.c_str());
            sd_audio_playing_.store(false);
            if (item.sequence) {
                item.sequence->success = false;
                if (item.last && item.sequence->completion) {
                    item.sequence->completion(false);
                    item.sequence->completion = {};
                }
            }
            continue;
        }

        const BaseType_t created = xTaskCreate(
            [](void* arg) {
                std::unique_ptr<PlaybackTaskContext> context(
                    static_cast<PlaybackTaskContext*>(arg));
                auto& app = *context->app;
                const int64_t playback_started_us = esp_timer_get_time();
                ESP_LOGI(TAG, "[SD_PLAY][START] path=%s generation=%u",
                         context->path.c_str(),
                         static_cast<unsigned>(context->generation));
                bool played = app.GetAudioService().PlayFile(context->path);
                app.GetAudioService().WaitForPlaybackIdle();
                const uint32_t finished_generation =
                    app.GetAudioService().GetFilePlaybackGeneration();
                const bool interrupted =
                    finished_generation != context->generation;
                played = played && !interrupted;
                const int64_t elapsed_us =
                    std::max<int64_t>(0, esp_timer_get_time() - playback_started_us);
                const long elapsed_ms = static_cast<long>(elapsed_us / 1000);
                if (played) {
                    app.RecordSdAudioPlaybackDuration(
                        static_cast<uint64_t>(elapsed_us / 1000));
                    ESP_LOGI(TAG,
                             "[SD_PLAY][DONE] path=%s elapsed_ms=%ld generation=%u",
                             context->path.c_str(), elapsed_ms,
                             static_cast<unsigned>(finished_generation));
                } else {
                    ESP_LOGE(TAG,
                             "[SD_PLAY][FAIL] path=%s elapsed_ms=%ld interrupted=%d start_generation=%u end_generation=%u",
                             context->path.c_str(), elapsed_ms,
                             interrupted,
                             static_cast<unsigned>(context->generation),
                             static_cast<unsigned>(finished_generation));
                }
                auto item = std::move(context->item);
                app.Schedule([played, item = std::move(item)]() mutable {
                    auto& scheduled_app = Application::GetInstance();
                    if (!played) {
                        Board::GetInstance().GetDisplay()->ShowNotification("SD audio play failed");
                    }
                    if (item.sequence) {
                        item.sequence->success = item.sequence->success && played;
                        if (item.last && item.sequence->completion) {
                            item.sequence->completion(item.sequence->success);
                            item.sequence->completion = {};
                        }
                    }
                    scheduled_app.sd_audio_playing_.store(false);
                    scheduled_app.StartNextSdAudioPlayback();
                });
                vTaskDelete(nullptr);
            },
            "play_sd_audio", 8192, context, 4, nullptr);

        if (created != pdPASS) {
            if (context->item.sequence) {
                context->item.sequence->success = false;
                if (context->item.last && context->item.sequence->completion) {
                    context->item.sequence->completion(false);
                    context->item.sequence->completion = {};
                }
            }
            ESP_LOGE(TAG, "[SD_PLAY][ERROR] task creation failed path=%s",
                     context->path.c_str());
            delete context;
            sd_audio_playing_.store(false);
            continue;
        }
        return;
    }

    if (sd_audio_pending_.exchange(false)) {
        guide_local_playback_active_ = false;
        display->SetPreviewImage(nullptr);
        display->SetContentFooter("");
        display->SetChatMessage("system", "");
        ESP_LOGI(TAG, "SD audio queue completed; power save countdown may resume");
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    }
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!CanStartPersonalChat()) return;
    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (sd_audio_playing_.load() || sd_audio_pending_.load())
        CancelSdAudioPlaybackOnMainTask();

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                SetDeviceState(kDeviceStateIdle);
                return;
            }
        }

        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!CanStartPersonalChat()) return;
    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (sd_audio_playing_.load() || sd_audio_pending_.load())
        CancelSdAudioPlaybackOnMainTask();

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                SetDeviceState(kDeviceStateIdle);
                return;
            }
        }

        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!CanStartPersonalChat() || !protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (sd_audio_playing_.load() || sd_audio_pending_.load())
        CancelSdAudioPlaybackOnMainTask();

    if (state == kDeviceStateIdle) {
        // Verified-binding devices do not upload pre-validation wake-word audio.
        if (!Board::GetInstance().RequiresUserBinding()) audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                SetDeviceState(kDeviceStateIdle);
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
        // Encode and send the wake word data to the server
        if (!Board::GetInstance().RequiresUserBinding()) {
            while (auto packet = audio_service_.PopWakeWordPacket()) {
                protocol_->SendAudio(std::move(packet));
            }
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        // Set flag to play popup sound after state changes to listening
        // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
        play_popup_on_listening_ = true;
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#endif
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            guide_speech_text_.clear();
            if (board.RequiresUserBinding() &&
                (!protocol_ || !protocol_->IsAudioChannelOpened())) {
                audio_service_.EnableVoiceProcessing(false);
                SetDeviceState(kDeviceStateIdle);
                break;
            }
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }

            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    tts_stop_pending_ = false;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

void Application::PrepareForShutdown() {
    ESP_LOGI(TAG, "Preparing application for shutdown...");
    aborted_ = true;
    audio_service_.CancelFilePlayback();
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    if (firmware_upgrade_in_progress_.exchange(true)) {
        ESP_LOGW(TAG, "Firmware upgrade already running");
        return false;
    }
    struct UpgradeGuard {
        std::atomic<bool>& flag;
        ~UpgradeGuard() { flag.store(false); }
    } guard{firmware_upgrade_in_progress_};
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, static_cast<unsigned>(speed / 1024));
        display->SetChatMessage("system", buffer);
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        SetDeviceState(kDeviceStateIdle);
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!CanStartPersonalChat() || !protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (sd_audio_playing_.load() || sd_audio_pending_.load())
        CancelSdAudioPlaybackOnMainTask();

    if (state == kDeviceStateIdle) {
        if (!Board::GetInstance().RequiresUserBinding()) audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                SetDeviceState(kDeviceStateIdle);
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        if (!Board::GetInstance().RequiresUserBinding()) {
            while (auto packet = audio_service_.PopWakeWordPacket()) {
                protocol_->SendAudio(std::move(packet));
            }
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        // Set flag to play popup sound after state changes to listening
        // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
        play_popup_on_listening_ = true;
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#endif
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (sd_audio_playing_.load() || sd_audio_pending_.load() ||
        (initial_group_announcement_requested_.load() &&
         !initial_group_announcement_completed_.load())) {
        return false;
    }

    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

bool Application::CanStartPersonalChat() {
    auto& board = Board::GetInstance();
    if (!board.RequiresUserBinding() || board.GetUserBinding().IsBound()) return true;
    ESP_LOGI(TAG, "Personal audio is waiting for a boot-scoped user binding");
    board.GetDisplay()->ShowNotification("请关联当前用户");
    return false;
}

void Application::ScheduleAudioChannelTask(std::function<void()>&& callback) {
    const uint32_t epoch = audio_channel_epoch_.load();
    Schedule([this, epoch, callback = std::move(callback)]() {
        if (epoch == audio_channel_epoch_.load() && protocol_ &&
            protocol_->IsAudioChannelOpened()) callback();
    });
}

void Application::VerifyUserBinding() {
    Schedule([this]() {
        auto& board = Board::GetInstance();
        if (!board.RequiresUserBinding() || !board.GetUserBinding().IsBound() ||
            !protocol_ || protocol_->IsAudioChannelOpened() ||
            GetDeviceState() != kDeviceStateIdle) return;
        // Verify without enabling capture or sending a listen/role request.
        protocol_->OpenAudioChannel();
    });
}

void Application::ApplyUserBinding(const UserBindingCommand& command,
    std::function<void(BindingCommandResult, const std::string&)> completion) {
    Schedule([this, command, completion = std::move(completion)]() {
        auto& board = Board::GetInstance();
        std::string error;
        auto result = board.CheckUserBindingCommand(command, error);
        if (result == BindingCommandResult::Accepted) {
            // Serialize close -> queue reset -> identity replacement on the main task.
            ++audio_channel_epoch_;
            audio_service_.EnableVoiceProcessing(false);
            if (protocol_) protocol_->CloseAudioChannel();
            if (!sd_audio_playing_.load()) audio_service_.ResetDecoder();
            play_popup_on_listening_ = false;
            return_idle_after_current_tts_ = false;
            role_switch_response_pending_ = false;
            role_switch_request_time_us_ = 0;
            role_switch_restore_pending_ = false;
            ResetRoleSwitchScan();
            CompleteRoleSwitch(false);
            const auto state = GetDeviceState();
            if (state == kDeviceStateListening || state == kDeviceStateSpeaking ||
                state == kDeviceStateConnecting) SetDeviceState(kDeviceStateIdle);
            board.GetDisplay()->SetChatMessage("system", "");
            result = board.ApplyUserBindingCommand(command, error);
        }
        if (completion) completion(result, error);
        if (result == BindingCommandResult::Accepted && !command.unbind) VerifyUserBinding();
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

