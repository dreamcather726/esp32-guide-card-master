#include "position_subscriber.h"
#include "position_mqtt_config.h"
#include "rndis_board.h"
#include "display.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>

namespace {
constexpr const char* kTag = "PositionMqtt";
constexpr int64_t kPositionTimeoutUs = 90LL * 1000 * 1000;
}

PositionSubscriber::PositionSubscriber(RndisBoard& board, std::function<bool()> network_ready)
    : board_(board), network_ready_(std::move(network_ready)) {}

PositionSubscriber::~PositionSubscriber() {
    stopping_ = true;
    // The owner first joins its existing worker, then stops MQTT callbacks.
    StopClient();
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
    ESP_LOGI(kTag, "[CONNECT] broker=%s clientId=%s username=%s password=<hidden> topic=%s keepalive=%d stack=%d buffer=%d internal_free=%u internal_largest=%u psram_free=%u",
             JIUCHUAN_POSITION_MQTT_URI, client_id_.c_str(), JIUCHUAN_POSITION_MQTT_USERNAME,
             topic_.c_str(), config.session.keepalive, config.task.stack_size, config.buffer.size,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    client_ = esp_mqtt_client_init(&config);
    if (!client_) {
        ESP_LOGE(kTag, "[INIT_FAILED] broker=%s internal_free=%u internal_largest=%u",
                 JIUCHUAN_POSITION_MQTT_URI,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        return false;
    }
    esp_err_t result = esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, Event, this);
    const char* stage = result == ESP_OK ? "create_task" : "register_event";
    if (result == ESP_OK) result = esp_mqtt_client_start(client_);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "[START_FAILED] broker=%s stage=%s error=%s stack=%d internal_free=%u internal_largest=%u; broker login has not started",
                 JIUCHUAN_POSITION_MQTT_URI, stage, esp_err_to_name(result), config.task.stack_size,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        esp_mqtt_client_destroy(client_); client_ = nullptr;
        return false;
    }
    ESP_LOGI(kTag, "[TASK_STARTED] broker=%s; waiting for broker CONNACK", JIUCHUAN_POSITION_MQTT_URI);
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

void PositionSubscriber::Poll() {
    if (stopping_) return;
    const bool configured = std::strlen(JIUCHUAN_POSITION_MQTT_PASSWORD) != 0;
    const int64_t now = esp_timer_get_time();
    const bool ready = board_.IsCellularNetwork() ? board_.IsNetworkReady() : network_ready_();
    // Signal strength does not depend on IMEI or MQTT authentication.
    if (!ready) {
        if (auto* display = board_.GetDisplay()) display->SetCellularSignal(-1);
        next_signal_ = 0;
    } else if (now >= next_signal_) {
        const bool valid = UpdateSignal();
        next_signal_ = now + (valid ? 30LL : 5LL) * 1000 * 1000;
    }
    const std::string imei = board_.GetImei();
    if (!ready || !ml307_at::IsImei(imei)) {
        if (client_) StopClient();
    } else {
        if (client_ && imei != imei_) {
            StopClient();
            next_start_ = 0;
        }
        if (configured && !client_ && now >= next_start_) {
            StartClient(imei);
            next_start_ = now + 5LL * 1000 * 1000;
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
}

void PositionSubscriber::Event(void* argument, esp_event_base_t, int32_t id, void* data) {
    static_cast<PositionSubscriber*>(argument)->HandleEvent(id, static_cast<esp_mqtt_event_handle_t>(data));
}

void PositionSubscriber::HandleEvent(int32_t id, esp_mqtt_event_handle_t event) {
    if (stopping_) return;
    if (id == MQTT_EVENT_BEFORE_CONNECT) {
        ESP_LOGI(kTag, "[LOGIN] broker=%s clientId=%s", JIUCHUAN_POSITION_MQTT_URI, client_id_.c_str());
    } else if (id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(kTag, "[CONNECTED] broker=%s clientId=%s CONNACK=0", JIUCHUAN_POSITION_MQTT_URI, client_id_.c_str());
        message_.Reset();
        const int message_id = esp_mqtt_client_subscribe(event->client, topic_.c_str(), 0);
        ESP_LOGI(kTag, "[SUBSCRIBE] broker=%s topic=%s message_id=%d", JIUCHUAN_POSITION_MQTT_URI, topic_.c_str(), message_id);
    } else if (id == MQTT_EVENT_SUBSCRIBED) {
        ESP_LOGI(kTag, "[SUBSCRIBED] broker=%s topic=%s message_id=%d", JIUCHUAN_POSITION_MQTT_URI, topic_.c_str(), event->msg_id);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        ESP_LOGW(kTag, "[DISCONNECTED] broker=%s; automatic reconnect in 5 seconds", JIUCHUAN_POSITION_MQTT_URI);
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
        if (event->error_handle) {
            const auto* error = event->error_handle;
            ESP_LOGE(kTag, "[ERROR] broker=%s type=%d connack=%d tls_esp=0x%x tls_stack=%d socket_errno=%d",
                     JIUCHUAN_POSITION_MQTT_URI, int(error->error_type), int(error->connect_return_code),
                     int(error->esp_tls_last_esp_err), error->esp_tls_stack_err, error->esp_transport_sock_errno);
        } else ESP_LOGE(kTag, "[ERROR] broker=%s (no error detail)", JIUCHUAN_POSITION_MQTT_URI);
    }
}
