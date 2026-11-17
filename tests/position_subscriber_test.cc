#include "../main/boards/jiuchuan-s3/position_data.h"
#include <atomic>
#include <functional>
#include <iostream>
#include <cassert>
#include <vector>
#include <mutex>
#include <cstdio>
#include <memory>

using esp_err_t = int;
constexpr int ESP_OK = 0, pdPASS = 1;
constexpr int pdTRUE = 1;
using TaskHandle_t = void*;
constexpr int MQTT_EVENT_ANY = -1, MQTT_EVENT_CONNECTED = 1, MQTT_EVENT_DISCONNECTED = 2,
              MQTT_EVENT_DATA = 3, MQTT_EVENT_ERROR = 4, MQTT_EVENT_BEFORE_CONNECT = 5,
              MQTT_EVENT_SUBSCRIBED = 6;
struct ErrorData {
    int error_type = 0, connect_return_code = 0, esp_tls_last_esp_err = 0,
        esp_tls_stack_err = 0, esp_transport_sock_errno = 0;
};
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
    int msg_id = 42;
    ErrorData* error_handle = nullptr;
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
static bool init_success = true;
static int register_result = ESP_OK, start_result = ESP_OK;
// Stack-only budget model: count the unchanged MQTT and manager stacks.
static int internal_budget = -1, internal_used = 0, internal_peak = 0;
static bool mqtt_stack_allocated = false;
static void (*event_callback)(void*, const char*, int32_t, void*) = nullptr;
static void* event_argument = nullptr;
static Client client;
static Client* esp_mqtt_client_init(const esp_mqtt_client_config_t* config) {
    assert(std::string(config->broker.address.uri) == "mqtt://example.test:1883");
    assert(std::string(config->credentials.username) == "unit-test");
    assert(std::string(config->credentials.authentication.password) == "fake-password");
    assert(config->task.stack_size == 4096); // Keep MQTT's original stack size.
    client_id = config->credentials.client_id;
    return init_success ? &client : nullptr;
}
static int esp_mqtt_client_register_event(Client*, int, decltype(event_callback) callback, void* argument) {
    event_callback = callback; event_argument = argument; return register_result;
}
static int esp_mqtt_client_start(Client*) {
    ++starts;
    if (start_result != ESP_OK) return start_result;
    if (internal_budget >= 0) {
        if (internal_used + 4096 > internal_budget) return -1;
        internal_used += 4096; internal_peak = std::max(internal_peak, internal_used);
        mqtt_stack_allocated = true;
    }
    return ESP_OK;
}
static int esp_mqtt_client_stop(Client*) { ++stops; return ESP_OK; }
static int esp_mqtt_client_destroy(Client*) {
    ++destroys;
    if (mqtt_stack_allocated) { internal_used -= 4096; mqtt_stack_allocated = false; }
    return ESP_OK;
}
static int esp_mqtt_client_subscribe(Client*, const char* value, int qos) {
    assert(qos == 0); topic = value; ++subscriptions; return 42;
}
static int64_t now_us = 1;
static int64_t esp_timer_get_time() { return now_us; }
static const char* esp_err_to_name(int) { return "error"; }
static std::string logs;
template <typename... Args> static void Log(const char*, const char* format, Args... args) {
    char buffer[2048];
    if constexpr (sizeof...(args) == 0) std::snprintf(buffer, sizeof(buffer), "%s", format);
    else std::snprintf(buffer, sizeof(buffer), format, args...);
    logs += buffer; logs += '\n';
}
constexpr unsigned MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_SPIRAM = 4;
static size_t heap_caps_get_free_size(unsigned caps) { return caps == MALLOC_CAP_SPIRAM ? 2000000 : 3500; }
static size_t heap_caps_get_largest_free_block(unsigned) { return 3000; }
static std::function<void()> on_delay;
static void vTaskDelay(int ms) { now_us += static_cast<int64_t>(ms) * 1000; if (on_delay) on_delay(); }
static std::function<void()> manager_task;
static int management_tasks = 0;
static void vTaskDelete(void*) { if (internal_budget >= 0) internal_used -= 12288; }
static int xTaskCreate(void (*entry)(void*), const char* name, int stack, void* argument, int, TaskHandle_t* handle) {
    assert(std::string(name) == "cabinet_mqtt" && stack == 12288);
    if (internal_budget >= 0 && internal_used + stack > internal_budget) return 0;
    ++management_tasks;
    if (internal_budget >= 0) { internal_used += stack; internal_peak = std::max(internal_peak, internal_used); }
    *handle = reinterpret_cast<void*>(1);
    manager_task = [=]() { entry(argument); };
    return pdPASS;
}
static int ulTaskNotifyTake(int, int ticks) { vTaskDelay(ticks); return 0; }
static void xTaskNotifyGive(TaskHandle_t) {}
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
#include "broker_log.inc"

constexpr uint32_t kRetryDelayMs = 2000, kUnauthorizedRetryDelayMs = 30000,
    kConnectWaitMs = 30000, kReconnectGraceMs = 60000, kOnlineReportIntervalSeconds = 60;
constexpr int kCredentialRefreshMarginSeconds = 300;
class SdContentManager {};
struct Application {
    static Application& GetInstance() { static Application instance; return instance; }
    void NotifyCustomerMqttOnline() {}
};
#define private public
#include "manager.inc"
#undef private
static int credential_requests = 0;
LocationMqttClient::LocationMqttClient(RndisBoard& board, std::function<bool()> ready)
    : board_(board), position_subscriber_(std::make_unique<PositionSubscriber>(board, std::move(ready))) {}
LocationMqttClient::~LocationMqttClient() { Stop(); }
bool LocationMqttClient::FetchCredentials(const std::string&, Credentials&) {
    ++credential_requests; last_credentials_http_status_ = 403; return false;
}
bool LocationMqttClient::StartMqtt(const Credentials&, const std::string&, const std::string&) { assert(false); return false; }
void LocationMqttClient::StopMqtt() {}
void LocationMqttClient::QuerySignalStrength() {}
bool LocationMqttClient::PublishOnlineReport() { return true; }
void LocationMqttClient::PersistRuntimeCheckpoint() {}

static void RunTicks(PositionSubscriber& subscriber) {
    while (!subscriber.stopping_) { subscriber.Poll(); vTaskDelay(1000); }
}

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
        RunTicks(subscriber); on_delay = {};
        assert(board.queries >= 3);
    }
    std::cout << "PASS shared-worker polling, regular signal refresh and 90-second stale telemetry expiry\n";
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
        RunTicks(subscriber); on_delay = {};
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
        RunTicks(subscriber); on_delay = {};
    }
    std::cout << "PASS signal refresh without IMEI and five-second retry after AT failure\n";
    {
        RndisBoard board;
        PositionSubscriber subscriber(board, []() { return true; });
        const int old_starts = starts, old_destroys = destroys;
        init_success = false;
        assert(!subscriber.StartClient(board.imei) && !subscriber.client_);
        assert(starts == old_starts && destroys == old_destroys);
        init_success = true; register_result = -1;
        assert(!subscriber.StartClient(board.imei) && !subscriber.client_);
        assert(starts == old_starts && destroys == old_destroys + 1);
        register_result = ESP_OK; start_result = -1;
        assert(!subscriber.StartClient(board.imei) && !subscriber.client_);
        assert(starts == old_starts + 1 && destroys == old_destroys + 2);
        assert(logs.find("stage=register_event") != std::string::npos);
        assert(logs.find("stage=create_task") != std::string::npos);
        assert(logs.find("internal_largest=3000") != std::string::npos);
        start_result = ESP_OK;
        assert(subscriber.StartClient(board.imei));
        EventData event; event.client = &client;
        subscriber.HandleEvent(MQTT_EVENT_BEFORE_CONNECT, &event);
        Connected(); subscriber.HandleEvent(MQTT_EVENT_SUBSCRIBED, &event);
        ErrorData error; error.error_type = 2; error.connect_return_code = 4;
        event.error_handle = &error; subscriber.HandleEvent(MQTT_EVENT_ERROR, &event);
        event.error_handle = nullptr; subscriber.HandleEvent(MQTT_EVENT_ERROR, &event);
        assert(logs.find("[LOGIN] broker=mqtt://example.test:1883") != std::string::npos);
        assert(logs.find("[CONNECTED]") != std::string::npos && logs.find("[SUBSCRIBED]") != std::string::npos);
        assert(logs.find("connack=4") != std::string::npos);
        assert(logs.find("fake-password") == std::string::npos);
        assert(logs.find("password=<hidden>") != std::string::npos);
        subscriber.StopClient();
        assert(BrokerAddressForLog("wss://username:secret@broker.example:443/path?token=secret#frag") == "wss://broker.example:443");
        assert(BrokerAddressForLog("wss://broker.example:443") == "wss://broker.example:443");
        assert(BrokerAddressForLog("wss://[::1]:443/mqtt") == "wss://[::1]:443");
    }
    std::cout << "PASS init/register/task failures clean up, retry succeeds, domain/heap/CONNACK logs and credential redaction\n";
    {
        RndisBoard board;
        PositionSubscriber subscriber(board, []() { return true; });
        const int original_starts = starts;
        start_result = -1;
        subscriber.Poll(); assert(starts == original_starts + 1 && !subscriber.client_);
        now_us += 4000000; subscriber.Poll(); assert(starts == original_starts + 1);
        start_result = ESP_OK;
        now_us += 1000000; subscriber.Poll();
        assert(starts == original_starts + 2 && subscriber.client_);
        board.ready = false; subscriber.Poll();
        assert(!subscriber.client_ && board.display.signal == -1);
    }
    std::cout << "PASS task-start failure retains five-second retry and network loss cleans up the recovered client\n";
    {
        RndisBoard board;
        // In the old layout, 12288+6144+4096 exceeded this 20000-byte stack budget.
        // The real shared Start/Run/WaitOrStop paths must now fit without shrinking any stack.
        internal_budget = 20000; internal_used = internal_peak = 0;
        internal_used = 12288 + 6144; // Previous pair of management stacks.
        assert(esp_mqtt_client_start(&client) != ESP_OK);
        internal_used = 0;
        const int original_tasks = management_tasks;
        const int original_requests = credential_requests;
        const int64_t begin = now_us;
        {
            LocationMqttClient manager(board, []() { return true; });
            int ticks = 0;
            on_delay = [&]() {
                ++ticks;
                if (ticks == 1) Connected();
                if (now_us - begin >= 95LL * 1000000) manager.stop_requested_ = true;
            };
            manager.Start(); manager.Start(); // Idempotent: only one existing management task.
            manager_task(); on_delay = {}; manager_task = {};
            assert(management_tasks == original_tasks + 1);
            assert(credential_requests - original_requests == 3); // 403 retry backoff 30s, 60s, 120s.
            assert(board.queries >= 4); // CSQ continues inside long credential backoff waits.
            assert(manager.position_subscriber_->client_ != nullptr);
            assert(internal_peak == 12288 + 4096 && internal_peak < internal_budget);
        }
        assert(internal_used == 0); // MQTT and shared worker stacks both released.
        internal_budget = -1;
    }
    std::cout << "PASS real shared manager Start/Run/WaitOrStop: one management task, 6KB stack saving, bounded stack-budget recovery, 403-independent positioning/CSQ and cleanup\n";
}
