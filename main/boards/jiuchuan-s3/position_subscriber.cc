#include "position_subscriber.h"
#include "position_mqtt_config.h"
#include "rndis_board.h"
#include "display.h"
#include <esp_log.h>
#include <esp_timer.h>

namespace {
constexpr const char* kTag = "PositionMqtt";
constexpr int64_t kPositionTimeoutUs = 90LL * 1000 * 1000;
}

PositionSubscriber::PositionSubscriber(RndisBoard& board, std::function<bool()> network_ready)
    : board_(board), network_ready_(std::move(network_ready)) {}

PositionSubscriber::~PositionSubscriber() {
    stopping_ = true;
    // Let the owner task stop/destroy MQTT and finish its AT query before
    // releasing callback state. No forced task deletion while it owns I/O.
    while (running_) vTaskDelay(pdMS_TO_TICKS(20));
}

void PositionSubscriber::Start() {
    if (running_.exchange(true)) return;
    stopping_ = false;
    if (xTaskCreate([](void* argument) {
            auto* self = static_cast<PositionSubscriber*>(argument);
            self->Run();
            self->running_ = false;
            vTaskDelete(nullptr);
        }, "position_mqtt", 6144, this, 2, nullptr) != pdPASS) {
        running_ = false;
        ESP_LOGE(kTag, "Cannot create position subscriber task");
    }
}

void PositionSubscriber::ClearPosition() {
    std::lock_guard<std::mutex> lock(navigation_mutex_);
    received_at_ = 0;
    if (auto* display = board_.GetDisplay()) display->SetNavigationStatus(NavigationStatus{});
}

bool PositionSubscriber::StartClient(const std::string& imei) {
    imei_ = imei;
    topic_ = "topic/" + imei + "/position";
    client_id_ = "s3-position-" + imei;
    esp_mqtt_client_config_t config = {};
    config.broker.address.uri = JIUCHUAN_POSITION_MQTT_URI;
    config.credentials.client_id = client_id_.c_str();
    config.credentials.username = JIUCHUAN_POSITION_MQTT_USERNAME;
    config.credentials.authentication.password = JIUCHUAN_POSITION_MQTT_PASSWORD;
    config.session.keepalive = 60;
    config.network.timeout_ms = 10000;
    config.network.reconnect_timeout_ms = 5000;
    config.buffer.size = 1024;
    config.task.stack_size = 4096;
    client_ = esp_mqtt_client_init(&config);
    if (!client_) return false;
    esp_err_t result = esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, Event, this);
    if (result == ESP_OK) result = esp_mqtt_client_start(client_);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "Position MQTT start failed: %s", esp_err_to_name(result));
        esp_mqtt_client_destroy(client_); client_ = nullptr;
        return false;
    }
    ESP_LOGI(kTag, "Position MQTT started for %s", topic_.c_str());
    return true;
}

void PositionSubscriber::StopClient() {
    if (client_) {
        esp_mqtt_client_stop(client_);
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
    }
    message_.Reset();
    ClearPosition();
}

bool PositionSubscriber::UpdateSignal() {
    int csq = -1;
    if (board_.IsCellularNetwork() && board_.IsNetworkReady()) {
        std::string response;
        if (board_.ExecuteUsbAtCommand("AT+CSQ", response, 3000)) csq = position::ParseCsq(response);
    }
    if (auto* display = board_.GetDisplay()) display->SetCellularSignal(csq);
    if (csq < 0 && board_.IsCellularNetwork() && board_.IsNetworkReady())
        ESP_LOGW(kTag, "CSQ unavailable; retrying in 5 seconds");
    return csq >= 0;
}

void PositionSubscriber::Run() {
    int64_t next_signal = 0, next_start = 0;
    const bool configured = std::strlen(JIUCHUAN_POSITION_MQTT_PASSWORD) != 0;
    if (!configured) ESP_LOGW(kTag, "Position credentials not configured; signal indicator remains available");
    while (!stopping_) {
        const int64_t now = esp_timer_get_time();
        const bool ready = board_.IsCellularNetwork() ? board_.IsNetworkReady() : network_ready_();
        // Signal strength does not depend on IMEI or MQTT authentication.
        if (!ready) {
            if (auto* display = board_.GetDisplay()) display->SetCellularSignal(-1);
            next_signal = 0;
        } else if (now >= next_signal) {
            const bool valid = UpdateSignal();
            next_signal = now + (valid ? 30LL : 5LL) * 1000 * 1000;
        }
        const std::string imei = board_.GetImei();
        if (!ready || !ml307_at::IsImei(imei)) {
            if (client_) StopClient();
        } else {
            if (client_ && imei != imei_) {
                StopClient();
                next_start = 0;
            }
            if (configured && !client_ && now >= next_start) {
                StartClient(imei);
                next_start = now + 5LL * 1000 * 1000;
            }
        }
        // A connected broker can silently stop receiving modem telemetry.
        {
            std::lock_guard<std::mutex> lock(navigation_mutex_);
            const int64_t last = received_at_;
            if (last > 0 && now - last >= kPositionTimeoutUs) {
                received_at_ = 0;
                if (auto* display = board_.GetDisplay()) display->SetNavigationStatus(NavigationStatus{});
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    StopClient();
}

void PositionSubscriber::Event(void* argument, esp_event_base_t, int32_t id, void* data) {
    static_cast<PositionSubscriber*>(argument)->HandleEvent(id, static_cast<esp_mqtt_event_handle_t>(data));
}

void PositionSubscriber::HandleEvent(int32_t id, esp_mqtt_event_handle_t event) {
    if (stopping_) return;
    if (id == MQTT_EVENT_CONNECTED) {
        message_.Reset();
        const int message_id = esp_mqtt_client_subscribe(event->client, topic_.c_str(), 0);
        ESP_LOGI(kTag, "Subscribe %s: message_id=%d", topic_.c_str(), message_id);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        message_.Reset();
        ClearPosition();
    } else if (id == MQTT_EVENT_DATA) {
        NavigationStatus status;
        if (message_.Feed(topic_, event->topic, event->topic_len, event->data, event->data_len,
                          event->current_data_offset, event->total_data_len, event->retain, status)) {
            std::lock_guard<std::mutex> lock(navigation_mutex_);
            received_at_ = esp_timer_get_time();
            if (auto* display = board_.GetDisplay()) display->SetNavigationStatus(status);
            ESP_LOGD(kTag, "Position received: fix=%d nsat=%d", status.has_fix, status.satellites);
        }
    } else if (id == MQTT_EVENT_ERROR) {
        ESP_LOGW(kTag, "Position MQTT error; automatic reconnect will retry");
    }
}
