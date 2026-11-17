#pragma once

#include "ota_download.h"
#include <cstring>
#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <nvs.h>

namespace ota_download {
class FlashSink final : public Sink {
public:
    FlashSink(const esp_partition_t* partition, const std::string& url) : partition_(partition) {
        mbedtls_sha256_init(&hash_);
        const auto scheme = url.find("://");
        const auto end = scheme == std::string::npos ? 0 : url.find('/', scheme + 3);
        const auto origin = url.substr(0, end);
        mbedtls_sha256(reinterpret_cast<const unsigned char*>(origin.data()), origin.size(), origin_, 0);
        nvs_open("ota_resume", NVS_READWRITE, &nvs_);
    }

    ~FlashSink() override {
        Abort();
        mbedtls_sha256_free(&hash_);
        if (nvs_) nvs_close(nvs_);
    }

    size_t Capacity() const override { return partition_->size; }

    bool Load(Image& image, size_t& offset) override {
        size_t length = sizeof(record_);
        if (!nvs_ || nvs_get_blob(nvs_, "state", &record_, &length) != ESP_OK ||
            length != sizeof(record_) || record_.format != 1 ||
            record_.partition != partition_->address ||
            memcmp(origin_, record_.origin, sizeof(origin_)) != 0 ||
            record_.etag[sizeof(record_.etag) - 1] != 0 ||
            record_.sha256[sizeof(record_.sha256) - 1] != 0) return false;
        image = {record_.size, record_.etag, record_.sha256};
        offset = record_.offset;
        if (!image.sha256.empty() && !HexDigest(image.sha256)) return false;
        return true;
    }

    bool Clear() override {
        if (!nvs_) return false;
        auto err = nvs_erase_key(nvs_, "state");
        if (err == ESP_ERR_NVS_NOT_FOUND) return true;
        return Check(err, "erase checkpoint") && Check(nvs_commit(nvs_), "commit erase");
    }

    bool Start(const Image& image, size_t offset) override {
        Abort();
        if (!partition_ || partition_ == esp_ota_get_running_partition() ||
            partition_->erase_size != kBlock || offset % kBlock != 0 ||
            image.size > Capacity()) return false;
        if (mbedtls_sha256_starts(&hash_, 0) != 0) return false;
        if (offset) {
            auto block = std::make_unique<std::array<unsigned char, kBlock>>();
            for (size_t pos = 0; pos < offset; pos += kBlock) {
                if (!Check(esp_partition_read(partition_, pos, block->data(), kBlock), "read prefix") ||
                    mbedtls_sha256_update(&hash_, block->data(), kBlock) != 0) return false;
            }
            unsigned char digest[32];
            if (!Digest(digest) || memcmp(digest, record_.prefix, sizeof(digest)) != 0) return false;
        }
        written_ = offset;
        ESP_LOGI("Ota", "[OTA_RESUME] partition=%s offset=%u total=%u", partition_->label,
                 static_cast<unsigned>(offset), static_cast<unsigned>(image.size));
        // Resuming at an erase boundary re-erases every uncommitted sector before writing.
        return Check(offset ? esp_ota_resume(partition_, OTA_WITH_SEQUENTIAL_WRITES, offset, &handle_)
                            : esp_ota_begin(partition_, OTA_WITH_SEQUENTIAL_WRITES, &handle_), "begin/resume");
    }

    bool Write(const char* data, size_t size) override {
        if (!written_) {
            constexpr size_t desc_offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
            if (size < desc_offset + sizeof(esp_app_desc_t)) return false;
            esp_app_desc_t desc;
            memcpy(&desc, data + desc_offset, sizeof(desc));
            if (static_cast<unsigned char>(data[0]) != ESP_IMAGE_HEADER_MAGIC ||
                desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
                memcmp(desc.project_name, esp_app_get_description()->project_name, sizeof(desc.project_name)) != 0) {
                ESP_LOGE("Ota", "[OTA_RESUME] invalid image header or different project");
                return false;
            }
            ESP_LOGI("Ota", "[OTA_RESUME] image version=%.32s", desc.version);
        }
        if (!Check(esp_ota_write(handle_, data, size), "write") ||
            mbedtls_sha256_update(&hash_, reinterpret_cast<const unsigned char*>(data), size) != 0) return false;
        written_ += size;
        return true;
    }

    bool Save(const Image& image, size_t offset) override {
        if (!nvs_ || offset != written_ || offset % kBlock || !StrongEtag(image.etag)) return false;
        Record next{};
        next.format = 1;
        next.partition = partition_->address;
        next.size = image.size;
        next.offset = offset;
        memcpy(next.origin, origin_, sizeof(origin_));
        memcpy(next.etag, image.etag.data(), image.etag.size());
        memcpy(next.sha256, image.sha256.data(), image.sha256.size());
        if (!Digest(next.prefix) || !Check(nvs_set_blob(nvs_, "state", &next, sizeof(next)), "save checkpoint") ||
            !Check(nvs_commit(nvs_), "commit checkpoint")) return false;
        record_ = next;
        ESP_LOGI("Ota", "[OTA_RESUME] saved offset=%u/%u", next.offset, next.size);
        return true;
    }

    void Abort() override {
        if (handle_) { esp_ota_abort(handle_); handle_ = 0; }
    }

    bool Finish(const Image& image) override {
        if (written_ != image.size) return false;
        if (!image.sha256.empty()) {
            unsigned char digest[32];
            if (!Digest(digest)) return false;
            char hex[65]{};
            constexpr char digits[] = "0123456789abcdef";
            for (size_t i = 0; i < sizeof(digest); ++i) {
                hex[i * 2] = digits[digest[i] >> 4];
                hex[i * 2 + 1] = digits[digest[i] & 15];
            }
            if (image.sha256 != hex) {
                ESP_LOGE("Ota", "[OTA_RESUME] SHA-256 mismatch; boot partition unchanged");
                return false;
            }
        }
        const auto err = esp_ota_end(handle_);
        handle_ = 0;  // esp_ota_end consumes the handle on both success and validation failure.
        if (!Check(err, "validate image")) return false;
        if (!Check(esp_ota_set_boot_partition(partition_), "set boot partition")) return false;
        ESP_LOGI("Ota", "[OTA_RESUME] image verified; firmware upgrade successful");
        return true;
    }

private:
    struct Record {
        uint32_t format, partition, size, offset;
        unsigned char origin[32], prefix[32];
        char etag[129], sha256[65];
    } record_{};
    const esp_partition_t* partition_;
    nvs_handle_t nvs_ = 0;
    esp_ota_handle_t handle_ = 0;
    mbedtls_sha256_context hash_{};
    unsigned char origin_[32]{};
    size_t written_ = 0;

    bool Digest(unsigned char output[32]) {
        mbedtls_sha256_context copy;
        mbedtls_sha256_init(&copy);
        mbedtls_sha256_clone(&copy, &hash_);
        const int result = mbedtls_sha256_finish(&copy, output);
        mbedtls_sha256_free(&copy);
        return result == 0;
    }
    static bool Check(esp_err_t err, const char* stage) {
        if (err == ESP_OK) return true;
        ESP_LOGE("Ota", "[OTA_RESUME] %s failed: %s", stage, esp_err_to_name(err));
        return false;
    }
};
}  // namespace ota_download
