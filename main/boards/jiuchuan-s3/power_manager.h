#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>

#include <esp_timer.h>
#include <driver/gpio.h>
#include "adc_battery_estimation.h"
#include "power_controller.h"
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#define JIUCHUAN_ADC_UNIT (ADC_UNIT_1)
#define JIUCHUAN_ADC_BITWIDTH (ADC_BITWIDTH_12)
#define JIUCHUAN_ADC_ATTEN (ADC_ATTEN_DB_12)
#define JIUCHUAN_ADC_CHANNEL (ADC_CHANNEL_3)
#define JIUCHUAN_RESISTOR_UPPER (200000)
#define JIUCHUAN_RESISTOR_LOWER (100000)

#undef TAG
#define TAG "PowerManager"
class PowerManager {
private:
    esp_timer_handle_t timer_handle_ = nullptr;
    std::function<void(bool)> on_charging_status_changed_;
    std::function<void(bool)> on_low_battery_status_changed_;
    gpio_num_t charging_pin_ = GPIO_NUM_NC;
    std::atomic<int32_t> battery_level_{-1};
    std::atomic<bool> is_charging_{false};
    bool is_low_battery_ = false;
    bool is_empty_battery_ = false;
    const int kLowBatteryLevel = 20;

    adc_battery_estimation_handle_t adc_battery_estimation_handle = nullptr;
    PowerController* power_controller_;



    void CheckBatteryStatus() {
        // Get charging status
        bool new_charging_status = charging_pin_ != GPIO_NUM_NC && gpio_get_level(charging_pin_) == 1;
        if (new_charging_status != is_charging_) {
            is_charging_ = new_charging_status;
            if (on_charging_status_changed_) {
                on_charging_status_changed_(is_charging_);
            }
        }
        ReadBatteryAdcData();
    }

    void ReadBatteryAdcData() {
        adc_battery_sample_t sample = {};
        const auto error = adc_battery_estimation_get_sample(adc_battery_estimation_handle, &sample);
        if (error != ESP_OK) {
            ESP_LOGW("PowerManager", "Battery ADC read failed: %s; keeping last valid level",
                     esp_err_to_name(error));
            return;
        }
        battery_level_ = static_cast<int32_t>(std::lround(std::clamp(sample.capacity, 0.0f, 100.0f)));
        ESP_LOGI("PowerManager",
                 "Battery level: %.1f%% voltage=%.3fV adc=%dmV curve=%.1f%% charge_pin=%d charging=%s",
                 std::clamp(sample.capacity, 0.0f, 100.0f), sample.battery_voltage,
                 sample.adc_voltage_mv, sample.voltage_capacity,
                 charging_pin_ == GPIO_NUM_NC ? -1 : gpio_get_level(charging_pin_),
                 charging_pin_ == GPIO_NUM_NC ? "unknown" : (is_charging_ ? "yes" : "no"));
    }

public:
    PowerManager(gpio_num_t pin) : charging_pin_(pin) {
        power_controller_ = &PowerController::Instance();
        // 初始化充电引脚
        if (charging_pin_ != GPIO_NUM_NC) {
            gpio_config_t io_conf = {};
            io_conf.intr_type = GPIO_INTR_DISABLE;
            io_conf.mode = GPIO_MODE_INPUT;
            io_conf.pin_bit_mask = (1ULL << charging_pin_);
            io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
            io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
            gpio_config(&io_conf);
        }

        // 创建电池电量检查定时器
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                PowerManager* self = static_cast<PowerManager*>(arg);
                self->CheckBatteryStatus();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_check_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));

        static const battery_point_t battery_points[] = {
            {3.7f, 100},
            {3.2f, 0},
        };

        adc_battery_estimation_t config = {
            .internal = {
                .adc_unit = JIUCHUAN_ADC_UNIT,
                .adc_bitwidth = JIUCHUAN_ADC_BITWIDTH,
                .adc_atten = JIUCHUAN_ADC_ATTEN,
            },
            .adc_channel = JIUCHUAN_ADC_CHANNEL,
            .upper_resistor = JIUCHUAN_RESISTOR_UPPER,
            .lower_resistor = JIUCHUAN_RESISTOR_LOWER,
            .battery_points = battery_points,
            .battery_points_count = sizeof(battery_points) / sizeof(battery_points[0]),
            .charging_detect_cb = charging_pin_ == GPIO_NUM_NC ? nullptr : +[](void* arg) {
                auto* self = static_cast<PowerManager*>(arg);
                return gpio_get_level(self->charging_pin_) == 1;
            },
            .charging_detect_user_data = this,
            .allow_bidirectional_capacity = charging_pin_ == GPIO_NUM_NC,
        };

        adc_battery_estimation_handle = adc_battery_estimation_create(&config);
        if (adc_battery_estimation_handle == nullptr) {
            ESP_LOGE("PowerManager", "Battery ADC initialization failed");
        } else {
            CheckBatteryStatus();
            ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 1000000));
        }

        RegisterAllCallbacks();
    }

    ~PowerManager() {
        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
        }
        if (adc_battery_estimation_handle) {
            adc_battery_estimation_destroy(adc_battery_estimation_handle);
        }
    }

    bool IsCharging() {
        // 如果电量已经满了，则不再显示充电中
        if (battery_level_ == 100) {
            //ESP_LOGI(TAG, "电量已满，不再显示充电中");
            return false;
        }
        return is_charging_;
    }

    bool IsDischarging() {
        // 没有区分充电和放电，所以直接返回相反状态
        return !is_charging_;
    }

    int32_t GetBatteryLevel() {
        return battery_level_;
    }

    void RegisterAllCallbacks() {
        //注册电源状态变更回调函数（优化版）
        power_controller_->OnStateChange([this](PowerState newState) {
            switch(newState) {
                case PowerState::SHUTDOWN: {

                    ESP_LOGD(TAG, "关机");
                    if (PWR_BUTTON_GPIO != GPIO_NUM_NC) {
                        // Prevent a shutdown wakeup from floating.
                        ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(PWR_BUTTON_GPIO, 0));
                        ESP_ERROR_CHECK(rtc_gpio_pulldown_en(PWR_BUTTON_GPIO));
                        ESP_ERROR_CHECK(rtc_gpio_pullup_dis(PWR_BUTTON_GPIO));
                    } else {
                        ESP_LOGI(TAG, "No dedicated power button; SAM8108 handles wakeup");
                    }
                    if (PWR_EN_GPIO != GPIO_NUM_NC) {
                        rtc_gpio_set_level(PWR_EN_GPIO, 0);
                        rtc_gpio_hold_dis(PWR_EN_GPIO);
                    }

                    // 确保所有外设已关闭
                    vTaskDelay(200 / portTICK_PERIOD_MS);
                    ESP_LOGI(TAG, "Initiating deep sleep");

                    esp_deep_sleep_start();
                    break;
                }
                default:
                    ESP_LOGD(TAG, "State changed to %d", static_cast<int>(newState));
                    break;
            }
        });
    }
    void SetPowerState(PowerState newState) {
        power_controller_->SetState(newState);
    }

    void OnLowBatteryStatusChanged(std::function<void(bool)> callback) {
        on_low_battery_status_changed_ = callback;
    }

    void OnChargingStatusChanged(std::function<void(bool)> callback) {
        on_charging_status_changed_ = callback;
    }
};
