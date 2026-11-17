#pragma once
#include <cstdint>
constexpr uint32_t ESP_APP_DESC_MAGIC_WORD = 0xabcd5432;
struct esp_app_desc_t { uint32_t magic_word; char version[32], project_name[32]; };
const esp_app_desc_t* esp_app_get_description();
