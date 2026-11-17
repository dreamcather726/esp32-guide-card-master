#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "system_info.h"

#define TAG "main"

extern "C" void app_main(void)
{
#if CONFIG_VANEX_RUNTIME_DIAGNOSTICS
    char elf_sha256[65] = {};
    esp_app_get_elf_sha256(elf_sha256, sizeof(elf_sha256));
    ESP_LOGI(TAG, "Diagnostic boot: version=%s reset_reason=%d ELF=%s ISR_stack=%d timer_stack=%d",
             esp_app_get_description()->version, static_cast<int>(esp_reset_reason()), elf_sha256,
             CONFIG_FREERTOS_ISR_STACKSIZE, CONFIG_ESP_TIMER_TASK_STACK_SIZE);
#endif
    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
