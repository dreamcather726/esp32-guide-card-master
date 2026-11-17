#include "ota_flash_sink.h"
#include <cassert>
#include <iostream>
#include <vector>

using namespace ota_download;
const esp_partition_t running{"ota_0", 0x20000, 0x3f0000, 4096};
const esp_partition_t target{"ota_1", 0x410000, 0x3f0000, 4096};
const esp_app_desc_t app{ESP_APP_DESC_MAGIC_WORD, "test", "xiaozhi"};
std::vector<unsigned char> flash(target.size, 0xff), committed, pending;
size_t written;
int active = 0, boot_changes = 0;
bool commit_ok = true, validate_ok = true;
const esp_partition_t* esp_ota_get_running_partition() { return &running; }
const esp_app_desc_t* esp_app_get_description() { return &app; }
esp_err_t esp_partition_read(const esp_partition_t*, size_t offset, void* data, size_t size) {
    assert(offset + size <= flash.size()); memcpy(data, flash.data() + offset, size); return ESP_OK;
}
esp_err_t esp_ota_resume(const esp_partition_t*, size_t erase, size_t offset, esp_ota_handle_t* handle) {
    assert(active == 0 && erase == OTA_WITH_SEQUENTIAL_WRITES && offset % 4096 == 0);
    *handle = 1; active = 1; written = offset; return ESP_OK;
}
esp_err_t esp_ota_begin(const esp_partition_t* p, size_t erase, esp_ota_handle_t* handle) {
    return esp_ota_resume(p, erase, 0, handle);
}
esp_err_t esp_ota_write(esp_ota_handle_t, const void* data, size_t size) {
    assert(active && written % 4096 == 0);
    std::fill(flash.begin() + written, flash.begin() + ((written + size + 4095) / 4096) * 4096, 0xff);
    memcpy(flash.data() + written, data, size); written += size; return ESP_OK;
}
esp_err_t esp_ota_abort(esp_ota_handle_t) { assert(active); active = 0; return ESP_OK; }
esp_err_t esp_ota_end(esp_ota_handle_t) { assert(active); active = 0; return validate_ok ? ESP_OK : -1; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*) { ++boot_changes; return ESP_OK; }
esp_err_t nvs_open(const char*, int, nvs_handle_t* handle) { *handle = 1; pending = committed; return ESP_OK; }
void nvs_close(nvs_handle_t) { pending = committed; }
esp_err_t nvs_get_blob(nvs_handle_t, const char*, void* data, size_t* size) {
    if (committed.empty()) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size >= committed.size()); *size = committed.size(); memcpy(data, committed.data(), *size); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void* data, size_t size) {
    auto bytes = static_cast<const unsigned char*>(data); pending.assign(bytes, bytes + size); return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t, const char*) { pending.clear(); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t) { if (!commit_ok) return -1; committed = pending; return ESP_OK; }

int main() {
    std::string data(65536 + 8192 + 17, 'x'); data[0] = ESP_IMAGE_HEADER_MAGIC;
    memcpy(data.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), &app, sizeof(app));
    unsigned char sha[32]; mbedtls_sha256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), sha, 0);
    std::string digest; constexpr char hex[] = "0123456789abcdef";
    for (auto byte : sha) { digest += hex[byte >> 4]; digest += hex[byte & 15]; }
    Image image{data.size(), "\"image\"", digest};
    auto save_prefix = [&]() {
        FlashSink sink(&target, "https://test/download/ticket1");
        assert(sink.Clear()); assert(sink.Start(image, 0));
        for (size_t i = 0; i < 65536; i += kBlock) assert(sink.Write(data.data() + i, kBlock));
        assert(sink.Save(image, 65536));
        assert(sink.Write(data.data() + 65536, kBlock));  // Not checkpointed before reset.
    };
    save_prefix(); assert(active == 0);
    {
        FlashSink sink(&target, "https://test/download/new-ticket"); Image restored; size_t offset;
        assert(sink.Load(restored, offset) && offset == 65536);
        assert(sink.Start(restored, offset));
        while (offset < data.size()) { auto n = std::min(kBlock, data.size() - offset); assert(sink.Write(data.data() + offset, n)); offset += n; }
        assert(sink.Finish(restored)); assert(boot_changes == 1);
        assert(memcmp(flash.data(), data.data(), data.size()) == 0);
    }
    save_prefix(); flash[1234] ^= 1;
    { FlashSink sink(&target, "https://test/download/a"); Image i; size_t offset;
      assert(sink.Load(i, offset)); assert(!sink.Start(i, offset)); assert(active == 0); }
    { FlashSink sink(&target, "https://different/download/a"); Image i; size_t o; assert(!sink.Load(i, o)); }
    { FlashSink sink(&running, "https://test/download/a"); Image i; size_t o; assert(!sink.Load(i, o)); assert(!sink.Start(image, 0)); }
    save_prefix();
    { FlashSink sink(&target, "https://test/download/a"); Image i; size_t offset;
      assert(sink.Load(i, offset)); assert(sink.Start(i, offset));
      assert(sink.Write(data.data() + offset, kBlock)); commit_ok = false;
      assert(!sink.Save(i, offset + kBlock)); commit_ok = true; }
    { FlashSink sink(&target, "https://test/download/a"); Image i; size_t offset;
      assert(sink.Load(i, offset) && offset == 65536); assert(sink.Start(i, offset)); }
    for (bool bad_hash : {false, true}) {
        FlashSink sink(&target, "https://test/download/a"); assert(sink.Clear()); assert(sink.Start(image, 0));
        for (size_t offset = 0; offset < data.size(); offset += kBlock)
            assert(sink.Write(data.data() + offset, std::min(kBlock, data.size() - offset)));
        auto invalid = image; if (bad_hash) invalid.sha256.assign(64, '0');
        validate_ok = bad_hash; assert(!sink.Finish(invalid)); sink.Abort(); assert(boot_changes == 1);
    }
    { FlashSink sink(&target, "https://test/download/a"); assert(sink.Start(image, 0));
      data[0] = 0; assert(!sink.Write(data.data(), kBlock)); }
    assert(active == 0);
    std::cout << "10 flash/NVS recovery scenarios passed (SHA-256 via OpenSSL; ESP flash/NVS mocked)\n";
}
