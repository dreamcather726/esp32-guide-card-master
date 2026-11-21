#ifndef BOARD_H
#define BOARD_H

#include <http.h>
#include <web_socket.h>
#include <mqtt.h>
#include <udp.h>
#include <string>
#include <functional>
#include <cstdint>
#include <vector>
#include <network_interface.h>

#include "led/led.h"
#include "backlight.h"
#include "camera.h"
#include "assets.h"
#include "user_binding.h"

/**
 * Network events for unified callback
 */
enum class NetworkEvent {
    Scanning,              // Network is scanning (WiFi scanning, etc.)
    Connecting,            // Network is connecting (data: SSID/network name)
    Connected,             // Network connected successfully (data: SSID/network name)
    Disconnected,          // Network disconnected
    WifiConfigModeEnter,   // Entered WiFi configuration mode
    WifiConfigModeExit,    // Exited WiFi configuration mode
    // Cellular modem specific events
    ModemDetecting,        // Detecting modem (baud rate, module type)
    ModemErrorNoSim,       // No SIM card detected
    ModemErrorRegDenied,   // Network registration denied
    ModemErrorInitFailed,  // Modem initialization failed
    ModemErrorTimeout      // Operation timeout
};

// Power save level enumeration
enum class PowerSaveLevel {
    LOW_POWER,    // Maximum power saving (lowest power consumption)
    BALANCED,     // Medium power saving (balanced)
    PERFORMANCE,  // No power saving (maximum power consumption / full performance)
};

// Network event callback type (event, data)
// data contains additional info like SSID for Connecting/Connected events
using NetworkEventCallback = std::function<void(NetworkEvent event, const std::string& data)>;

struct IndexedRgbColor {
    uint16_t index = 0;  // Customer protocol uses one-based LED indexes.
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
};

void* create_board();
class AudioCodec;
class Display;
class Board {
private:
    Board(const Board&) = delete; // 禁用拷贝构造函数
    Board& operator=(const Board&) = delete; // 禁用赋值操作

protected:
    Board();
    std::string GenerateUuid();

    // 软件生成的设备唯一标识
    std::string uuid_;
    BootUserBinding user_binding_;

public:
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;
    virtual std::string GetBoardType() = 0;
    virtual std::string GetUuid() { return uuid_; }
    virtual bool RequiresUserBinding() const { return false; }
    virtual bool IsStartupPresentationPending() const { return false; }
    virtual std::string GetUserGuid() { return GetUserBinding().user_guid; }
    UserBindingSnapshot GetUserBinding() const { return user_binding_.Get(); }
    std::string GetBootId() const { return GetUserBinding().boot_id; }
    BindingCommandResult CheckUserBindingCommand(const UserBindingCommand& command,
                                                std::string& error) const {
        return user_binding_.Check(command, error);
    }
    BindingCommandResult ApplyUserBindingCommand(const UserBindingCommand& command,
                                                std::string& error) {
        auto result = user_binding_.Apply(command, error);
        if (result == BindingCommandResult::Accepted) OnUserBindingChanged();
        return result;
    }
    bool SetUserBindingState(const UserBindingSnapshot& expected, BindingState state) {
        const bool updated = user_binding_.SetState(expected, state);
        if (updated) OnUserBindingChanged();
        return updated;
    }
    virtual void OnUserBindingChanged() {}
    virtual Backlight* GetBacklight() { return nullptr; }
    virtual Led* GetLed();
    virtual AudioCodec* GetAudioCodec() = 0;
    virtual bool GetTemperature(float& esp32temp);
    virtual Display* GetDisplay();
    virtual Camera* GetCamera();
    virtual NetworkInterface* GetNetwork() = 0;
    virtual void StartNetwork() = 0;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) { (void)callback; }
    virtual const char* GetNetworkStateIcon() = 0;
    virtual bool IsCellularNetwork() const { return false; }
    virtual bool ConfigureNetwork(const std::string& type,
                                  const std::string& wifi_credentials,
                                  bool apply,
                                  bool& reboot_required,
                                  std::string& error) {
        (void)type;
        (void)wifi_credentials;
        (void)apply;
        reboot_required = false;
        error = "network switching is unsupported on this board";
        return false;
    }
    virtual bool RequestShutdown(std::string& error) {
        error = "remote shutdown is unsupported on this board";
        return false;
    }
    virtual bool ApplyRgbControl(
            bool update_rgb1, const std::vector<IndexedRgbColor>& rgb1,
            bool update_rgb2, const std::vector<IndexedRgbColor>& rgb2,
            std::string& error) {
        (void)update_rgb1;
        (void)rgb1;
        (void)update_rgb2;
        (void)rgb2;
        error = "indexed RGB control is unsupported on this board";
        return false;
    }
    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging);
    virtual std::string GetSystemInfoJson();
    virtual void SetPowerSaveLevel(PowerSaveLevel level) = 0;
    virtual std::string GetBoardJson() = 0;
    virtual std::string GetDeviceStatusJson() = 0;
};

#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}

#endif // BOARD_H
