#include "websocket_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"
#include "user_binding_json.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "WS"

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
}

WebsocketProtocol::~WebsocketProtocol() {
    CloseAudioChannel();
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    // Only connect to server when audio channel is needed
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!IsAudioChannelOpened()) {
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (!IsAudioChannelOpened()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send websocket text");
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    if (!channel_ready_ || context_failed_ || websocket_ == nullptr ||
        !websocket_->IsConnected() || error_occurred_ || IsTimeout()) return false;
    if (!binding_required_) return true;
    const auto binding = Board::GetInstance().GetUserBinding();
    return binding.state == BindingState::Ready && binding.SameIdentity(channel_binding_);
}

void WebsocketProtocol::CloseAudioChannel() {
    ++channel_generation_;
    {
        std::lock_guard<std::mutex> lock(context_mutex_);
        channel_ready_ = false;
        if (binding_required_) {
            auto& board = Board::GetInstance();
            if (board.GetUserBinding().state != BindingState::Error) {
                board.SetUserBindingState(channel_binding_, BindingState::Pending);
            }
        }
    }
    const bool had_channel = websocket_ != nullptr;
    websocket_.reset();
    if (had_channel && on_audio_channel_closed_) on_audio_channel_closed_();
}

bool WebsocketProtocol::OpenAudioChannel() {
    CloseAudioChannel();
    auto& board = Board::GetInstance();
    binding_required_ = board.RequiresUserBinding();
    channel_binding_ = board.GetUserBinding();
    if (binding_required_ && !channel_binding_.IsBound()) {
        ESP_LOGW(TAG, "Audio channel requires a boot-scoped binding");
        return false;
    }
    if (binding_required_) board.SetUserBindingState(channel_binding_, BindingState::Pending);
    const uint32_t generation = ++channel_generation_;
    context_failed_ = false;
    session_id_.clear();
    xEventGroupClearBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT |
        WEBSOCKET_PROTOCOL_USER_CONTEXT_EVENT | WEBSOCKET_PROTOCOL_CONTEXT_ERROR_EVENT);
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }

    error_occurred_ = false;

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (binding_required_) {
        websocket_->SetHeader("User-Binding", "1");
        websocket_->SetHeader("User-Guid", channel_binding_.user_guid.c_str());
        websocket_->SetHeader("Boot-Id", channel_binding_.boot_id.c_str());
        websocket_->SetHeader("Binding-Version", std::to_string(channel_binding_.version).c_str());
        websocket_->SetHeader("Binding-Token", channel_binding_.token.c_str());
        websocket_->SetHeader("Usage-Session-Id", channel_binding_.usage_session_id.c_str());
    }

    websocket_->OnData([this, generation](const char* data, size_t len, bool binary) {
        if (generation != channel_generation_.load()) return;
        last_incoming_time_ = std::chrono::steady_clock::now();
        if (binary) {
            if (!IsAudioChannelOpened()) return;
            if (on_incoming_audio_ != nullptr) {
                if (version_ == 2) {
                    BinaryProtocol2* bp2 = (BinaryProtocol2*)data;
                    bp2->version = ntohs(bp2->version);
                    bp2->type = ntohs(bp2->type);
                    bp2->timestamp = ntohl(bp2->timestamp);
                    bp2->payload_size = ntohl(bp2->payload_size);
                    auto payload = (uint8_t*)bp2->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = bp2->timestamp,
                        .payload = std::vector<uint8_t>(payload, payload + bp2->payload_size)
                    }));
                } else if (version_ == 3) {
                    BinaryProtocol3* bp3 = (BinaryProtocol3*)data;
                    bp3->type = bp3->type;
                    bp3->payload_size = ntohs(bp3->payload_size);
                    auto payload = (uint8_t*)bp3->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + bp3->payload_size)
                    }));
                } else {
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)
                    }));
                }
            }
        } else {
            // Parse JSON data
            auto root = cJSON_ParseWithLength(data, len);
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else if (strcmp(type->valuestring, "user_context") == 0 && binding_required_) {
                    ParseUserContext(root);
                } else {
                    if (IsAudioChannelOpened() && on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Missing websocket message type");
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this, generation]() {
        std::lock_guard<std::mutex> lock(context_mutex_);
        if (generation != channel_generation_.load()) return;
        channel_ready_ = false;
        context_failed_ = true;
        xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_CONTEXT_ERROR_EVENT);
        if (binding_required_ && Board::GetInstance().GetUserBinding().state != BindingState::Error) {
            Board::GetInstance().SetUserBindingState(channel_binding_, BindingState::Pending);
        }
        ESP_LOGI(TAG, "Websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d", websocket_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        CloseAudioChannel();
        return false;
    }

    // Send hello message to describe the client
    auto message = GetHelloMessage();
    // Only hello bypasses the verified-context send gate.
    if (!websocket_->Send(message)) {
        SetError(Lang::Strings::SERVER_ERROR);
        CloseAudioChannel();
        return false;
    }

    // One deadline for hello AND user_context; either arrival order is supported.
    const EventBits_t required = WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT |
        (binding_required_ ? WEBSOCKET_PROTOCOL_USER_CONTEXT_EVENT : 0);
    EventBits_t received = 0;
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(binding_required_ ? 30000 : 10000);
    while ((received & required) != required) {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout) break;
        received |= xEventGroupWaitBits(event_group_handle_, required |
            WEBSOCKET_PROTOCOL_CONTEXT_ERROR_EVENT, pdTRUE, pdFALSE, timeout - elapsed);
        if (received & WEBSOCKET_PROTOCOL_CONTEXT_ERROR_EVENT) break;
    }
    if ((received & required) != required || context_failed_) {
        if (binding_required_) FailUserContext();
        else SetError(Lang::Strings::SERVER_TIMEOUT);
        CloseAudioChannel();
        return false;
    }
    bool validated = false;
    {
        std::lock_guard<std::mutex> lock(context_mutex_);
        validated = !context_failed_ && websocket_->IsConnected() &&
            generation == channel_generation_.load() &&
            (!binding_required_ || board.SetUserBindingState(channel_binding_, BindingState::Ready));
        channel_ready_ = validated;
    }
    if (!validated) {
        FailUserContext();
        CloseAudioChannel();
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

std::string WebsocketProtocol::GetHelloMessage() {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    if (binding_required_) cJSON_AddBoolToObject(features, "user_binding", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    if (binding_required_) {
        cJSON_AddStringToObject(root, "userGUID", channel_binding_.user_guid.c_str());
        cJSON_AddStringToObject(root, "bootID", channel_binding_.boot_id.c_str());
        cJSON_AddNumberToObject(root, "bindingVersion", static_cast<double>(channel_binding_.version));
        cJSON_AddStringToObject(root, "bindingToken", channel_binding_.token.c_str());
        cJSON_AddStringToObject(root, "usageSessionID", channel_binding_.usage_session_id.c_str());
    }
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport) || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported websocket transport");
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}

void WebsocketProtocol::ParseUserContext(const cJSON* root) {
    if (context_failed_) return;
    if (!IsReadyUserContext(root, channel_binding_)) {
        FailUserContext();
        return;
    }
    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_USER_CONTEXT_EVENT);
}

void WebsocketProtocol::FailUserContext() {
    std::lock_guard<std::mutex> lock(context_mutex_);
    context_failed_ = true;
    channel_ready_ = false;
    Board::GetInstance().SetUserBindingState(channel_binding_, BindingState::Error);
    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_CONTEXT_ERROR_EVENT);
    ESP_LOGW(TAG, "User context validation failed or timed out; personal audio blocked");
    SetError("User binding validation failed");
}
