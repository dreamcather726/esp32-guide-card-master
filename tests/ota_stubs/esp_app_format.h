#pragma once
constexpr unsigned char ESP_IMAGE_HEADER_MAGIC = 0xe9;
struct esp_image_header_t { unsigned char data[24]; };
struct esp_image_segment_header_t { unsigned char data[8]; };
