#include "../main/boards/jiuchuan-s3/position_data.h"
#include <atomic>
#include <functional>
#include <iostream>
#include <cassert>
#include <vector>
#include <mutex>

using esp_err_t = int;
constexpr int ESP_OK = 0, pdPASS = 1;
constexpr int MQTT_EVENT_ANY = -1, MQTT_EVENT_CONNECTED = 1, MQTT_EVENT_DISCONNECTED = 2,
              MQTT_EVENT_DATA = 3, MQTT_EVENT_ERROR = 4;
using esp_event_base_t = const char*;
struct Client {};
using esp_mqtt_client_handle_t = Client*;
struct EventData {
    Client* client = nullptr;
    char* topic = nullptr;
    int topic_len = 0;
    char* data = nullptr;
    int data_len = 0, current_data_offset = 0, total_data_len = 0;
    bool retain = false;
};
using esp_mqtt_event_handle_t = EventData*;
struct esp_mqtt_client_config_t {
    struct { struct { const char* uri = nullptr; } address; } broker;
    struct { const char* client_id = nullptr; const char* username = nullptr;
             struct { const char* password = nullptr; } authentication; } credentials;
    struct { int keepalive = 0; } session;
    struct { int timeout_ms = 0, reconnect_timeout_ms = 0; } network;
    struct { int size = 0; } buffer;
    struct { int stack_size = 0; } task;
};
static std::string topic, client_id;
static int starts = 0, stops = 0, destroys = 0, subscriptions = 0;
static void (*event_callback)(void*, const char*, int32_t, void*) = nullptr;
static void* event_argument = nullptr;
static Client client;
static Client* esp_mqtt_client_init(const esp_mqtt_client_config_t* config) {
    assert(std::string(config->broker.address.uri) == "mqtt://example.test:1883");
    assert(std::string(config->credentials.username) == "unit-test");
    assert(std::string(config->credentials.authentication.password) == "fake-password");
    client_id = config->credentials.client_id;
    return &client;
}
static int esp_mqtt_client_register_event(Client*, int, decltype(event_callback) callback, void* argument) {
    event_callback = callback; event_argument = argument; return ESP_OK;
}
static int esp_mqtt_client_start(Client*) { ++starts; return ESP_OK; }
static int esp_mqtt_client_stop(Client*) { ++stops; return ESP_OK; }
static int esp_mqtt_client_destroy(Client*) { ++destroys; return ESP_OK; }
static int esp_mqtt_client_subscribe(Client*, const char* value, int qos) {
    assert(qos == 0); topic = value; ++subscriptions; return 42;
}
static int64_t now_us = 1;
static int64_t esp_timer_get_time() { return now_us; }
static const char* esp_err_to_name(int) { return "error"; }
template <typename... Args> static void Log(const char*, const char*, Args...) {}
static std::function<void()> on_delay;
static void vTaskDelay(int ms) { now_us += static_cast<int64_t>(ms) * 1000; if (on_delay) on_delay(); }
static void vTaskDelete(void*) {}
static int xTaskCreate(void (*)(void*), const char*, int, void*, int, void*) { return pdPASS; }
struct Display {
    NavigationStatus navigation;
    int signal = -1, updates = 0;
    void SetNavigationStatus(const NavigationStatus& status) { navigation = status; ++updates; }
    void SetCellularSignal(int csq) { signal = csq; }
};
namespace ml307_at {
static bool IsImei(const std::string& value) { return value.size() == 15; }
}
struct RndisBoard {
    Display display;
    bool cellular = true, ready = true;
    std::string imei = "864479086534998";
    int queries = 0;
    bool at_success = true;
    Display* GetDisplay() { return &display; }
    bool IsCellularNetwork() const { return cellular; }
    bool IsNetworkReady() const { return ready; }
    std::string GetImei() const { return imei; }
    bool ExecuteUsbAtCommand(const std::string& command, std::string& response, int timeout) {
        assert(command == "AT+CSQ" && timeout == 3000); ++queries; response = "+CSQ: 28,99\r\n"; return at_success;
    }
};
#define ESP_LOGI Log
#define ESP_LOGW Log
#define ESP_LOGD Log
#define ESP_LOGE Log
#define pdMS_TO_TICKS(ms) (ms)
#define JIUCHUAN_POSITION_MQTT_URI "mqtt://example.test:1883"
#define JIUCHUAN_POSITION_MQTT_USERNAME "unit-test"
#define JIUCHUAN_POSITION_MQTT_PASSWORD "fake-password"
#define private public
#include "subscriber.inc"
#undef private

static void Connected() {
    EventData event; event.client = &client;
    event_callback(event_argument, nullptr, MQTT_EVENT_CONNECTED, &event);
}
static void Payload(const std::string& body, bool retained = false) {
    EventData event; event.client = &client;
    event.topic = topic.data(); event.topic_len = topic.size(); event.data = const_cast<char*>(body.data());
    event.data_len = event.total_data_len = body.size(); event.retain = retained;
    event_callback(event_argument, nullptr, MQTT_EVENT_DATA, &event);
}

int main() {
    {
        RndisBoard board;
        PositionSubscriber subscriber(board, []() { return true; });
        assert(subscriber.StartClient(board.imei)); Connected();
        assert(topic == "topic/864479086534998/position");
        assert(client_id == "s3-position-864479086534998");
        const auto payload = R"({"fix":1,"latitude":"4003.8","longitude":"11625.2","nsat":"15","Scene":"24812"})";
        Payload(payload, true); assert(board.display.updates == 0);
        Payload(payload); assert(board.display.navigation.has_fix && board.display.navigation.satellites == 15);
        assert(board.display.navigation.scene == "24812");
        EventData event; event.client = &client;
        event_callback(event_argument, nullptr, MQTT_EVENT_DISCONNECTED, &event);
        assert(!board.display.navigation.has_fix && board.display.navigation.satellites == -1);
        Connected(); assert(subscriptions == 2);
        Payload(payload); assert(board.display.navigation.satellites == 15);
        subscriber.UpdateSignal(); assert(board.display.signal == 28);
        subscriber.StopClient(); assert(stops == 1 && destroys == 1);
    }
    std::cout << "PASS native MQTT config, own IMEI topic, retained rejection, disconnect clear, reconnect subscribe and AT CSQ\n";
    {
        RndisBoard board;
        PositionSubscriber subscriber(board, []() { return true; });
        int ticks = 0;
        on_delay = [&]() {
            ++ticks;
            if (ticks == 1) {
                Connected(); Payload(R"({"fix":1,"latitude":40,"longitude":116,"nsat":15,"Scene":"24812"})");
            }
            if (ticks == 90) assert(board.display.navigation.satellites == 15);
            if (ticks == 92) {
                assert(board.display.navigation.satellites == -1 && board.display.navigation.scene.empty());
                subscriber.stopping_ = true;
            }
        };
        subscriber.Run(); on_delay = {};
        assert(board.queries >= 3);
    }
    std::cout << "PASS independent subscriber loop, regular signal refresh and 90-second stale telemetry expiry\n";
    {
        RndisBoard board; board.cellular = false;
        bool wifi_ready = false;
        PositionSubscriber subscriber(board, [&]() { return wifi_ready; });
        const int original_starts = starts;
        int ticks = 0;
        on_delay = [&]() {
            ++ticks;
            if (ticks == 1) { assert(starts == original_starts); wifi_ready = true; }
            if (ticks == 2) { assert(starts == original_starts + 1); Connected(); board.imei = "864479086534999"; }
            if (ticks == 3) {
                Connected(); assert(topic == "topic/864479086534999/position"); subscriber.stopping_ = true;
            }
        };
        subscriber.Run(); on_delay = {};
        assert(board.queries == 0);
    }
    std::cout << "PASS WiFi readiness gate and changed physical IMEI restarts subscription\n";
    {
        RndisBoard board; board.imei.clear(); board.at_success = false;
        PositionSubscriber subscriber(board, []() { return true; });
        const int original_starts = starts;
        int ticks = 0;
        on_delay = [&]() {
            ++ticks;
            if (ticks == 1) { assert(board.queries == 1); assert(board.display.signal == -1); board.at_success = true; }
            if (ticks == 7) {
                assert(board.queries == 2 && board.display.signal == 28);
                assert(starts == original_starts); subscriber.stopping_ = true;
            }
        };
        subscriber.Run(); on_delay = {};
    }
    std::cout << "PASS signal refresh without IMEI and five-second retry after AT failure\n";
}
