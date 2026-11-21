// Production worker methods are included by run_rndis_host_tests.py.
// Only the USB and RTOS boundaries are simulated here.
#include "ml307_at.h"
#include <functional>
#include <iostream>
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
constexpr uint32_t kUsbEnumerationTimeoutMs = 12000;
constexpr uint32_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
struct WorkerIdle {};

struct Environment {
    EventBits_t bits = EVENT_USB_DEVICE_BIT | EVENT_GOT_IP_BIT;
    int waits = 0;
    unsigned probes = 0, probe_failures = 0;
    unsigned enumeration_timeouts = 0;
    std::function<void()> on_missing_modem;
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
class RndisBoard {
public:
    std::mutex usb_state_mutex_;
    uint32_t usb_session_ = 1;
    uint8_t usb_device_addr_ = 1;
    bool usb_at_dial_succeeded_ = false;
    void* usb_at_port_ = nullptr;
    void* s_event_group = nullptr;
    bool IsNetworkReady() const { return (env->bits & EVENT_GOT_IP_BIT) != 0; }
    void OnEnumerationTimeout() {
        ++env->enumeration_timeouts;
        if (env->on_missing_modem) env->on_missing_modem();
    }
    bool EnsureUsbAtControlPort(uint32_t session) {
        ++env->probes;
        if (env->probe_failures) { --env->probe_failures; return false; }
        if (!usb_device_addr_ || session != usb_session_) return false;
        usb_at_port_ = this;
        return true;
    }
    bool SendUsbAtCommand(const std::string& cmd, std::string& response, uint32_t, uint32_t session) {
        response.clear();
        if (session != usb_session_ || !usb_at_port_) return false;
        env->commands.push_back(cmd);
        if (env->on_command) env->on_command(cmd);
        response = "\r\nOK\r\n";
        return session == usb_session_ && usb_at_port_;
    }
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
    test("warm modem already has IP", [](auto& board) {
        Run(board);
        Check(env->probes == 0 && env->commands.empty(), "online modem was probed or redialed");
    });
    test("IP arrives during startup grace", [](auto& board) {
        env->bits = EVENT_USB_DEVICE_BIT;
        env->on_wait = [](auto mask, auto) {
            if (mask == (EVENT_GOT_IP_BIT | EVENT_USB_GONE_BIT)) env->bits |= EVENT_GOT_IP_BIT;
        };
        Run(board);
        Check(env->probes == 0 && env->commands.empty(), "grace path redialed");
    });
    test("cold modem dials and gets IP", [](auto& board) {
        env->bits = EVENT_USB_DEVICE_BIT;
        env->on_command = [](const auto& command) {
            if (command == "AT+MDIALUP=1,1") env->bits |= EVENT_GOT_IP_BIT;
        };
        Run(board);
        Check(Calls("AT+MDIALUP=1,1") == 1 && board.IsNetworkReady(), "cold start failed");
    });
    test("AT probe retries without another USB event", [](auto& board) {
        env->bits = EVENT_USB_DEVICE_BIT;
        env->probe_failures = 2;
        env->on_command = [](const auto& command) {
            if (command == "AT+MDIALUP=1,1") env->bits |= EVENT_GOT_IP_BIT;
        };
        Run(board);
        Check((env->retry_delays == std::vector<uint32_t>{2000, 4000}), "wrong probe backoff");
        Check(env->probes == 3 && board.IsNetworkReady(), "probe retry did not recover");
    });
    test("dial accepted without DHCP is retried after 30 seconds", [](auto& board) {
        env->bits = EVENT_USB_DEVICE_BIT;
        env->on_command = [](const auto& command) {
            if (command == "AT+MDIALUP=1,1" && Calls(command) == 2) env->bits |= EVENT_GOT_IP_BIT;
        };
        Run(board);
        Check(Calls("AT+MDIALUP=1,1") == 2, "missing DHCP recovery");
        Check((env->retry_delays == std::vector<uint32_t>{30000}), "wrong DHCP retry interval");
    });
    test("no attached USB device reports timeout and stays idle", [](auto& board) {
        board.usb_device_addr_ = 0;
        env->bits = 0;
        Run(board);
        Check(env->commands.empty() && env->probes == 0, "probing absent device");
        Check(env->enumeration_timeouts == 1 && env->retry_delays == std::vector<uint32_t>{12000},
              "startup did not report the missing modem once");
    });
    test("late enumeration recovers after timeout", [](auto& board) {
        board.usb_device_addr_ = 0;
        env->bits = 0;
        env->on_missing_modem = [&]() {
            board.usb_device_addr_ = 1;
            env->bits = EVENT_USB_DEVICE_BIT | EVENT_GOT_IP_BIT;
        };
        Run(board);
        Check(env->enumeration_timeouts == 1 && board.IsNetworkReady(), "late device did not recover");
        Check(env->commands.empty(), "late warm modem was redialed");
    });
    test("USB re-enumeration cancels an old dial attempt", [](auto& board) {
        env->bits = 0;
        env->on_command = [&](const auto& command) {
            if (command == "AT+CPIN?") ++board.usb_session_;
        };
        Check(!board.DetectAndDialUsbAt(), "stale dial attempt succeeded");
        Check(Calls("AT+MDIALUP=1,1") == 0 && !board.usb_at_dial_succeeded_, "stale session dialed");
    });
    std::cout << passed << " production flow scenarios passed\n";
}
