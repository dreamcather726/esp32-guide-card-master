#pragma once
#include <mutex>
#include <functional>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_log.h>
#include "config.h"
enum class PowerState {
        ACTIVE,
        LIGHT_SLEEP,
        DEEP_SLEEP,
        SHUTDOWN
    };

class PowerController {
public:


    static PowerController& Instance() {
        static PowerController instance;
        return instance;
    }

    void SetState(PowerState newState) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (currentState_ != newState) {
            ESP_LOGI("PowerCtrl", "State change: %d -> %d",
                    static_cast<int>(currentState_),
                    static_cast<int>(newState));

            currentState_ = newState;
            if (stateChangeCallback_) {
                stateChangeCallback_(newState);
            }
        }
    }

    PowerState GetState() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return currentState_;
    }

    void OnStateChange(std::function<void(PowerState)> callback) {
        stateChangeCallback_ = callback;
    }

private:
    PowerController(){
        // GPIO5 was removed from the revised board. The SAM8108 now owns the
        // system/modem power rail, so GPIO_NUM_NC must remain a true no-op.
        if (PWR_EN_GPIO == GPIO_NUM_NC) {
            ESP_LOGI("PowerCtrl", "External power control is handled by SAM8108");
            return;
        }
        rtc_gpio_init(PWR_EN_GPIO);
        rtc_gpio_set_direction(PWR_EN_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(PWR_EN_GPIO, 1);
    }
    ~PowerController() = default;

    PowerState currentState_ = PowerState::ACTIVE;
    std::function<void(PowerState)> stateChangeCallback_;
    mutable std::mutex mutex_;
};
