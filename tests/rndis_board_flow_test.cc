// Production worker/identity methods are included by run_rndis_host_tests.py.
// Only the USB, RTOS and NVS boundaries are simulated here.
#include "ml307_at.h"
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define EVENT_GOT_IP_BIT 1
#define EVENT_USB_DEVICE_BIT 2
#define EVENT_USB_GONE_BIT 4
#define EVENT_USB_AT_RX_BIT 8
#define EVENT_USB_MAINTENANCE_BIT 16
using TickType_t = uint32_t;
using EventBits_t = uint32_t;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr bool pdTRUE = true, pdFALSE = false;
constexpr uint32_t kExistingRndisGracePeriodMs = 8000;
constexpr uint32_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
struct WorkerIdle {};

struct Environment {
    EventBits_t bits = EVENT_USB_DEVICE_BIT | EVENT_GOT_IP_BIT;
    int waits = 0;
    unsigned probes = 0, probe_failures = 0, imei_failures = 0;
    bool cellular = true;
    std::string csq_reply = "\r\n+CSQ: 28,99\r\nOK\r\n";
    unsigned connecting = 0;
    std::map<std::string, std::string> nvs;
    std::vector<std::string> commands;
    std::vector<uint32_t> retry_delays;
    std::function<void(EventBits_t, uint32_t)> on_wait;
    std::function<void(const std::string&)> on_command;
};
static Environment* env = nullptr;
EventBits_t xEventGroupWaitBits(void*, EventBits_t mask, bool clear, bool, uint32_t wait) {
    if (++env->waits > 30) throw std::runtime_error("worker failed to become idle");
    if (!(env->bits & mask)) {
        if (wait == portMAX_DELAY) throw WorkerIdle{};
        if (mask == (EVENT_USB_DEVICE_BIT | EVENT_USB_MAINTENANCE_BIT)) {
            env->retry_delays.push_back(wait);
        }
        if (env->on_wait) env->on_wait(mask, wait);
    }
    const auto result = env->bits;
    if (clear) env->bits &= ~mask;
    return result;
}
void vTaskDelay(uint32_t) {}
bool ParseCregRegistration(const std::string&, int& status) { status = 1; return true; }
struct Settings {
    Settings(const char*, bool) {}
    std::string GetString(const std::string& key) { return env->nvs[key]; }
    void SetString(const std::string& key, const std::string& value) { env->nvs[key] = value; }
};
enum class NetworkEvent { Connecting };
class RndisBoard {
public:
    std::mutex usb_state_mutex_;
    uint32_t usb_session_ = 1;
    uint8_t usb_device_addr_ = 1;
    bool usb_at_dial_succeeded_ = false;
    void* usb_at_port_ = nullptr;
    void* s_event_group = nullptr;
    std::string imei_;
    bool monitor_signal_ = false;
    int cellular_csq_ = -1;
    int GetCellularSignal() const { return cellular_csq_; }
    bool IsCellularNetwork() const { return env->cellular; }
    bool IsNetworkReady() const { return (env->bits & EVENT_GOT_IP_BIT) != 0; }
    void OnNetworkEvent(NetworkEvent, const std::string&) { ++env->connecting; }
    bool EnsureUsbAtControlPort(uint32_t session) {
        ++env->probes;
        if (env->probe_failures) { --env->probe_failures; return false; }
        if (!usb_device_addr_ || session != usb_session_) return false;
        usb_at_port_ = this;
        return true;
    }
    bool SendUsbAtCommand(const std::string& cmd, std::string& response, uint32_t, uint32_t session) {
        env->commands.push_back(cmd);
        response.clear();
        if (session != usb_session_ || !usb_at_port_) return false;
        if (env->on_command) env->on_command(cmd);
        if (cmd == "AT+CSQ") {
            response = env->csq_reply;
            return !response.empty();
        } else if (cmd == "AT+CGSN=1" || cmd == "AT+CGSN" || cmd == "AT+GSN") {
            if (env->imei_failures) { --env->imei_failures; return false; }
            response = "\r\n864479086486322\r\nOK\r\n";
        } else {
            response = "\r\nOK\r\n";
        }
        return true;
    }
    std::string GetImei();
    bool QueryAndCacheImei(uint32_t session);
    bool DetectAndDialUsbAt();
    void NetworkTask();
};
#include "rndis_production.inc"

static void Check(bool value, const char* what) {
    if (!value) throw std::runtime_error(what);
}
static size_t Calls(const std::string& command) {
    return std::count(env->commands.begin(), env->commands.end(), command);
}
static void Run(RndisBoard& board) {
    try { board.NetworkTask(); } catch (const WorkerIdle&) {}
}

int main() {
    unsigned passed = 0;
    auto test = [&](const char* name, auto body) {
        Environment context;
        env = &context;
        RndisBoard board;
        body(board);
        ++passed;
        std::cout << "PASS " << name << '\n';
    };
    test("warm modem, blank NVS, IP already ready", [](auto& board) {
        Run(board);
        Check(board.GetImei() == "864479086486322", "IMEI missing");
        Check(env->nvs["imei"] == board.GetImei(), "identity not persisted");
        Check(Calls("AT+MDIALUP=1,1") == 0 && Calls("AT+CPIN?") == 0, "online modem was redialed");
        Check(env->connecting == 0, "ready UI regressed");
    });
    test("IP arrives during startup grace", [](auto& board) {
        env->bits = EVENT_USB_DEVICE_BIT;
        env->on_wait = [](auto mask, auto) {
            if (mask == (EVENT_GOT_IP_BIT | EVENT_USB_GONE_BIT)) env->bits |= EVENT_GOT_IP_BIT;
        };
        Run(board);
        Check(!board.GetImei().empty() && Calls("AT+MDIALUP=1,1") == 0, "grace path skipped identity");
    });
    test("AT probe fails then recovers without another USB event", [](auto& board) {
        env->probe_failures = 2;
        Run(board);
        Check(!board.GetImei().empty(), "retry did not obtain identity");
        Check((env->retry_delays == std::vector<uint32_t>{2000, 4000}), "wrong probe backoff");
        Check(Calls("AT+MDIALUP=1,1") == 0, "probe retry redialed");
    });
    test("IMEI fails then recovers without another USB event", [](auto& board) {
        env->imei_failures = 3;
        Run(board);
        Check(!board.GetImei().empty(), "IMEI retry missing");
        Check((env->retry_delays == std::vector<uint32_t>{2000}), "wrong IMEI backoff");
    });
    test("cold modem follows normal dial path", [](auto& board) {
        env->bits = EVENT_USB_DEVICE_BIT;
        env->on_command = [](const auto& command) {
            if (command == "AT+MDIALUP=1,1") env->bits |= EVENT_GOT_IP_BIT;
        };
        Run(board);
        Check(Calls("AT+MDIALUP=1,1") == 1 && !board.GetImei().empty(), "cold start failed");
    });
    test("signal is sampled before IMEI, DHCP and MQTT", [](auto& board) {
        board.monitor_signal_ = true;
        env->bits = EVENT_USB_DEVICE_BIT;
        env->imei_failures = 3;
        env->on_command = [&](const std::string& command) {
            if (command == "AT+CGSN=1" && Calls(command) == 1) {
                Check(!board.IsNetworkReady() && board.GetImei().empty(), "test skipped early startup");
                Check(board.GetCellularSignal() == 28, "early CSQ not yet available");
                throw WorkerIdle{};
            }
        };
        Run(board);
        Check(Calls("AT+CSQ") == 1, "startup never sampled CSQ");
    });
    test("online CSQ follows signal changes and retries failures", [](auto& board) {
        board.monitor_signal_ = true;
        env->on_wait = [&](auto, auto wait) {
            const auto sample = Calls("AT+CSQ");
            if (sample == 1) {
                Check(board.GetCellularSignal() == 28 && wait == 10000, "strong signal not sampled");
                env->csq_reply = "+CSQ: 6,99\r\nOK\r\n";
            } else if (sample == 2) {
                Check(board.GetCellularSignal() == 6 && wait == 10000, "weak signal not updated");
                env->csq_reply.clear();
            } else if (sample == 3) {
                Check(board.GetCellularSignal() == -1 && wait == 5000, "failed read did not clear and retry");
                env->csq_reply = "+CSQ: 18,99\r\nOK\r\n";
            } else {
                Check(sample == 4 && board.GetCellularSignal() == 18, "signal did not recover");
                throw WorkerIdle{};
            }
        };
        Run(board);
        Check(Calls("AT+CGSN=1") == 1 && Calls("AT+MDIALUP=1,1") == 0,
              "signal polling interfered with the online connection");
    });
    test("cached dial flag does not skip missing identity", [](auto& board) {
        board.usb_at_dial_succeeded_ = true;
        Run(board);
        Check(!board.GetImei().empty(), "dial flag suppressed identity query");
    });
    test("already-read identity is not queried repeatedly", [](auto& board) {
        board.imei_ = "864479086486322";
        Run(board);
        Check(env->commands.empty(), "healthy session generated commands");
    });
    test("old modem NVS is withheld in cellular mode", [](auto& board) {
        env->nvs["imei"] = "111111111111111";
        Check(board.GetImei().empty(), "stale identity exposed");
        Run(board);
        Check(board.GetImei() == "864479086486322", "identity not refreshed");
    });
    test("WiFi cached identity remains supported", [](auto& board) {
        env->cellular = false;
        env->nvs["imei"] = "864479086486322";
        Check(board.GetImei() == "864479086486322", "WiFi fallback lost");
        env->nvs["imei"] = "invalid";
        Check(board.GetImei().empty(), "invalid WiFi identity accepted");
    });
    test("re-enumeration rejects an old IMEI response", [](auto& board) {
        board.usb_at_port_ = &board;
        env->on_command = [&](const auto&) { ++board.usb_session_; };
        Check(!board.QueryAndCacheImei(1), "stale response accepted");
        Check(board.imei_.empty() && env->nvs.empty(), "stale response persisted");
    });
    test("no attached USB device stays idle", [](auto& board) {
        board.usb_device_addr_ = 0;
        Run(board);
        Check(env->commands.empty() && env->probes == 0, "probing absent device");
    });
    test("dial accepted but no IP is retried after 30 seconds", [](auto& board) {
        env->bits = EVENT_USB_DEVICE_BIT;
        env->on_command = [](const auto& command) {
            if (command == "AT+MDIALUP=1,1" && Calls(command) == 2) env->bits |= EVENT_GOT_IP_BIT;
        };
        Run(board);
        Check(Calls("AT+MDIALUP=1,1") == 2, "missing DHCP recovery");
        Check((env->retry_delays == std::vector<uint32_t>{30000}), "unbounded dial loop");
    });
    std::cout << passed << " production flow scenarios passed\n";
}
