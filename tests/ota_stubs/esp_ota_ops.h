#pragma once
#include <cstdint>
#include <cstddef>
using esp_err_t = int;
using esp_ota_handle_t = unsigned;
constexpr int ESP_OK = 0;
constexpr int ESP_ERR_NVS_NOT_FOUND = 1;
constexpr size_t OTA_WITH_SEQUENTIAL_WRITES = size_t(-2);
struct esp_partition_t { const char* label; uint32_t address, size, erase_size; };
const esp_partition_t* esp_ota_get_running_partition();
esp_err_t esp_partition_read(const esp_partition_t*, size_t, void*, size_t);
esp_err_t esp_ota_begin(const esp_partition_t*, size_t, esp_ota_handle_t*);
esp_err_t esp_ota_resume(const esp_partition_t*, size_t, size_t, esp_ota_handle_t*);
esp_err_t esp_ota_write(esp_ota_handle_t, const void*, size_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*);
inline const char* esp_err_to_name(int) { return "stub error"; }
