// Production methods are included verbatim. HTTP, JPEG decoding and timing are fake.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include "../main/boards/jiuchuan-s3/startup_qr_jpeg.h"

namespace fs = std::filesystem;
static const std::string kImei = "864479086534998";
static const std::string kPath = "sdcard/" + kImei + ".jpg";
static const std::string kPartial = "sdcard/." + kImei + ".jpg.part";
static const std::string kUrl = "https://iotossbj.suji.work/SDfile/QR/" + kImei + ".jpg";
static std::vector<uint32_t> delays;
static std::function<void()> on_delay;
static bool fail_sync = false, fail_write = false, fail_rename = false;
static int writes_before_failure = -1;
static int no_memory_decodes = 0;
static size_t decoded_width = 300;
static int decode_calls = 0;
static std::string cached_imei;
static std::string configured_imei;
struct Settings {
    Settings(const char*, bool) {}
    std::string GetString(const char*) const { return cached_imei; }
};

static void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template <typename... Args>
static void Log(const char*, const char*, Args...) {}
static void vTaskDelay(uint32_t delay) {
    delays.push_back(delay);
    Check(delays.size() < 100, "QR worker did not stop");
    if (on_delay) on_delay();
}
static int TestFsync(int fd) {
    if (fail_sync) { errno = EIO; return -1; }
    return ::fsync(fd);
}
static size_t TestWrite(const void* data, size_t size, size_t count, FILE* file) {
    if (fail_write || writes_before_failure == 0) { errno = ENOSPC; return 0; }
    if (writes_before_failure > 0) --writes_before_failure;
    return std::fwrite(data, size, count, file);
}

// A real SOF layout exercises the production header parser. The fake decoder
// accepts the default fixture; ESP entropy decoding needs a real-device test.
static std::string Jpeg(uint16_t width = 300, uint16_t height = 300) {
    const uint8_t bytes[] = {0xff, 0xd8, 0xff, 0xc0, 0, 17, 8,
        static_cast<uint8_t>(height >> 8), static_cast<uint8_t>(height),
        static_cast<uint8_t>(width >> 8), static_cast<uint8_t>(width),
        3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1, 0xff, 0xd9};
    return {reinterpret_cast<const char*>(bytes), sizeof(bytes)};
}
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = -2;
constexpr int LV_COLOR_FORMAT_RGB565 = 1;
using esp_err_t = int;
static const char* esp_err_to_name(int result) { return result == ESP_ERR_NO_MEM ? "NO_MEM" : "FAIL"; }
static void* heap_caps_malloc(size_t size, int) { return std::malloc(size); }
static void heap_caps_free(void* data) { std::free(data); }
static int jpeg_to_image(const uint8_t* data, size_t size, uint8_t** pixels,
                         size_t* bytes, size_t* width, size_t* height, size_t* stride) {
    ++decode_calls;
    if (no_memory_decodes > 0) { --no_memory_decodes; return ESP_ERR_NO_MEM; }
    if (std::string(reinterpret_cast<const char*>(data), size) != Jpeg()) return ESP_FAIL;
    *pixels = static_cast<uint8_t*>(std::malloc(2));
    *bytes = 2; *width = decoded_width; *height = 300; *stride = decoded_width * 2;
    return ESP_OK;
}
struct LvglImage { virtual ~LvglImage() = default; };
struct LvglAllocatedImage : LvglImage {
    void* pixels;
    LvglAllocatedImage(void* data, size_t, int, int, int, int) : pixels(data) {}
    ~LvglAllocatedImage() override { heap_caps_free(pixels); }
};
#include "../main/display/diagnostic_channel.h"
struct NoEmojiSpiLcdDisplay {
    int shows = 0;
    std::unique_ptr<LvglImage> image;
    std::string diagnostic;
    std::vector<size_t> written_progress;
    size_t progress_total = 0;
    bool progress_visible = false, progress_verified = false;
    void SetQrDownloadProgress(size_t written, size_t total, bool verifying = false) {
        progress_visible = true;
        progress_total = total;
        if (verifying) progress_verified = true;
        else written_progress.push_back(written);
    }
    void HideQrDownloadProgress() { progress_visible = false; }
    void SetDiagnostic(DiagnosticChannel channel, const char* value) {
        Check(channel == DiagnosticChannel::Qr, "wrong QR diagnostic channel");
        diagnostic = value;
    }
    void SetDiagnosticStatus(DiagnosticChannel channel, const char* value) {
        SetDiagnostic(channel, value);
    }
    void SetStartupQrImage(std::unique_ptr<LvglImage> value, const std::string& imei) {
        Check(imei == kImei, "startup display IMEI mismatch");
        Check(!progress_visible, "progress covered the displayed QR");
        ++shows; image = std::move(value);
    }
};
struct FakePowerSaveTimer {
    int wake_count = 0;
    void WakeUp() { ++wake_count; }
};
struct Application {
    static Application& GetInstance() { static Application app; return app; }
    void Schedule(std::function<void()> callback) { callback(); }
};
namespace ml307_at {
static bool IsImei(const std::string& imei) {
    return imei.size() == 15 && std::all_of(imei.begin(), imei.end(),
        [](char ch) { return ch >= '0' && ch <= '9'; });
}
}
struct Response {
    std::string body = Jpeg();
    bool open = true;
    int status = 200;
    size_t expected = Jpeg().size();
    int read_error_after = -1;
    size_t max_chunk = 4096;
};
struct FakeHttp {
    Response response;
    size_t offset = 0;
    void SetTimeout(int timeout) { Check(timeout == 15000, "HTTP timeout"); }
    bool Open(const char* method, const std::string& url) {
        Check(std::string(method) == "GET" && url == kUrl, "wrong device QR URL");
        return response.open;
    }
    int GetStatusCode() const { return response.status; }
    size_t GetBodyLength() const { return response.expected; }
    int Read(char* buffer, size_t capacity) {
        if (response.read_error_after >= 0 && offset >= static_cast<size_t>(response.read_error_after)) return -1;
        size_t size = std::min({capacity, response.max_chunk, response.body.size() - offset});
        if (response.read_error_after >= 0) size = std::min(size, static_cast<size_t>(response.read_error_after) - offset);
        std::memcpy(buffer, response.body.data() + offset, size);
        offset += size;
        return static_cast<int>(size);
    }
    void Close() {}
};
struct FakeNetwork {
    std::vector<Response> responses{Response{}};
    size_t requests = 0;
    std::unique_ptr<FakeHttp> CreateHttp(int id) {
        Check(id == 4, "QR must use its own HTTP connection ID");
        Check(requests < responses.size(), "unexpected extra QR HTTP request");
        auto http = std::make_unique<FakeHttp>();
        http->response = responses[requests++];
        return http;
    }
};

namespace std {
static int TestRename(const char* source, const char* destination) {
    if (fail_rename) { errno = EIO; return -1; }
    return std::rename(source, destination);
}
using ::TestWrite;
}
#define BOARD_TAG "QRTest"
#define SDCARD_MOUNT_POINT "sdcard"
#define ESP_LOGI Log
#define ESP_LOGW Log
#define ESP_LOGE Log
#define pdMS_TO_TICKS(ms) (ms)
#define fsync TestFsync
#define fwrite TestWrite
#define rename TestRename
#define JIUCHUAN_STARTUP_QR_IMEI configured_imei
struct TestBoard {
    std::atomic<bool> startup_pending_{true};
    std::atomic<bool> startup_transport_started_{true}, startup_network_ready_{true};
    int network_starts = 0, qr_failures = 0;
    void RequestStartupNetwork() { ++network_starts; startup_transport_started_ = true; }
    void FailStartupQr() { ++qr_failures; }
    enum class TfDownloadResult { Success, Retry, StorageError };
    NoEmojiSpiLcdDisplay display;
    NoEmojiSpiLcdDisplay* display_ = &display;
    FakePowerSaveTimer power_timer;
    FakePowerSaveTimer* power_save_timer_ = &power_timer;
    FakeNetwork network;
    bool network_present = true, cellular_network_ = true, ready = true;
    int pending_waits = 0, wait_calls = 0;
    std::string imei = kImei;
    std::vector<std::string> storage_errors;
    std::string GetImei() const { return imei; }
    NoEmojiSpiLcdDisplay* GetDisplay() { return display_; }
    FakeNetwork* GetNetwork() { return network_present ? &network : nullptr; }
    bool IsNetworkReady() const { return ready; }
    bool WaitForNetworkReady(int timeout) {
        Check(timeout == 30000, "QR network wait timeout");
        ++wait_calls;
        if (pending_waits-- > 0) return false;
        ready = true;
        return true;
    }
    void ShowInternalStorageError(const std::string& error) { storage_errors.push_back(error); }
#include "tf_qr_production.inc"
};
#undef fsync
#undef fwrite
#undef rename
#undef JIUCHUAN_STARTUP_QR_IMEI

static void Write(const fs::path& path, const std::string& data) {
    std::ofstream file(path, std::ios::binary);
    file.write(data.data(), data.size());
    Check(file.good(), "fixture write failed");
}
static std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
static void Ready(TestBoard& b, size_t requests = 1) {
    Check(Read(kPath) == Jpeg(), "wrong QR saved in TF root");
    Check(b.display.shows == 1 && b.display.image != nullptr, "QR not displayed exactly once");
    Check(b.network.requests == requests, "wrong HTTP request count");
    Check(b.storage_errors.empty(), "unexpected storage error");
    Check(b.power_timer.wake_count == 1, "QR readiness did not wake the screen");
}
static int tests = 0;
static void Run(const char* name, const std::function<void(TestBoard&)>& test) {
    const auto previous = fs::current_path();
    const auto folder = previous / ("qr_case_" + std::to_string(++tests));
    fs::create_directory(folder);
    fs::current_path(folder);
    fs::create_directory("sdcard");
    delays.clear(); on_delay = {}; fail_sync = fail_write = fail_rename = false;
    writes_before_failure = -1;
    no_memory_decodes = 0; decoded_width = 300; decode_calls = 0;
    cached_imei.clear();
    configured_imei.clear();
    TestBoard board;
    test(board);
    fs::current_path(previous);
    std::cout << "PASS " << name << '\n';
}
int main() {
    Run("missing QR downloaded to IMEI.jpg and displayed", [](TestBoard& b) {
        b.EnsureTfStartupQr(); Ready(b);
        Check(!fs::exists(kPartial), "partial file remains");
    });
    Run("QR progress tracks successful TF writes before verification", [](TestBoard& b) {
        b.network.responses[0].max_chunk = 10;
        b.EnsureTfStartupQr(); Ready(b);
        Check(b.display.written_progress == std::vector<size_t>{0, 10, 20, Jpeg().size()},
              "progress did not follow written bytes");
        Check(b.display.progress_total == Jpeg().size() && b.display.progress_verified &&
              !b.display.progress_visible, "progress did not switch to verification and close");
    });
    Run("QR write failure hides progress without counting the failed chunk", [](TestBoard& b) {
        b.network.responses[0].max_chunk = 10;
        writes_before_failure = 1;
        b.EnsureTfStartupQr();
        Check(b.display.written_progress == std::vector<size_t>{0, 10} &&
              !b.display.progress_visible && !b.display.progress_verified,
              "failed write left incorrect or visible progress");
        Check(b.display.diagnostic.rfind("QR TF WRITE E", 0) == 0 && b.display.shows == 0,
              "progress hid the write failure");
    });
    Run("cached QR displayed offline before USB IP readiness", [](TestBoard& b) {
        Write(kPath, Jpeg()); b.ready = false; b.network_present = false;
        b.EnsureTfStartupQr(); Ready(b, 0);
        Check(b.wait_calls == 0, "cached QR waited for network");
    });
    Run("QR received after explicit startup completion cannot reopen splash", [](TestBoard& b) {
        b.startup_pending_ = false;
        b.EnsureTfStartupQr();
        Check(Read(kPath) == Jpeg() && b.network.requests == 1, "late download not cached");
        Check(b.display.shows == 0 && b.power_timer.wake_count == 0, "late download interrupted normal use");
    });
    Run("other device QR and unrelated files preserved", [](TestBoard& b) {
        Write("sdcard/000000000000000.jpg", "other device");
        Write("sdcard/start.wav", "existing start audio");
        b.EnsureTfStartupQr(); Ready(b);
        Check(Read("sdcard/000000000000000.jpg") == "other device", "other QR changed");
        Check(Read("sdcard/start.wav") == "existing start audio", "audio changed");
    });
    Run("interrupted staging file overwritten", [](TestBoard& b) {
        Write(kPartial, "incomplete"); b.EnsureTfStartupQr(); Ready(b);
        Check(!fs::exists(kPartial), "staging file remains");
    });
    for (const auto& content : {std::string{}, std::string("HTML error"), Jpeg().substr(0, 4),
                               std::string("\xff\xd8" "bad" "\xff\xd9", 7)}) {
        Run("empty, invalid, truncated or undecodable JPEG repaired", [content](TestBoard& b) {
            Write(kPath, content); b.EnsureTfStartupQr(); Ready(b);
        });
    }
    Run("delayed IMEI postpones path and request selection", [](TestBoard& b) {
        b.imei.clear();
        on_delay = [&]() { b.imei = kImei; };
        b.EnsureTfStartupQr(); Ready(b);
        Check(delays.size() == 1, "IMEI readiness was not awaited");
    });
    Run("configured IMEI downloads on IP readiness without a live AT identity", [](TestBoard& b) {
        configured_imei = kImei; b.imei.clear(); b.ready = false; b.pending_waits = 1;
        on_delay = [&]() {
            Check(b.network.requests == 0 && b.display.diagnostic == "QR WAIT NETWORK",
                  "configured IMEI must wait for IP");
        };
        b.EnsureTfStartupQr(); Ready(b);
        Check(b.wait_calls == 2 && b.imei.empty(), "fallback changed the modem identity");
    });
    Run("configured IMEI reuses its saved image offline", [](TestBoard& b) {
        configured_imei = kImei; b.imei.clear(); b.ready = false;
        Write(kPath, Jpeg());
        b.EnsureTfStartupQr(); Ready(b, 0);
        Check(b.network_starts == 0 && b.wait_calls == 0, "saved QR waited for IP");
    });
    Run("live modem IMEI takes priority over the configured QR fallback", [](TestBoard& b) {
        configured_imei = "111111111111111";
        b.imei.clear(); b.ready = false; b.pending_waits = 1;
        on_delay = [&]() { b.imei = kImei; };
        b.EnsureTfStartupQr(); Ready(b);
    });
    Run("long cellular startup retries without blocking local tasks", [](TestBoard& b) {
        b.ready = false; b.pending_waits = 16;
        b.EnsureTfStartupQr(); Ready(b);
        Check(b.wait_calls == 17, "cellular startup retry stopped early");
    });
    Run("WiFi bypasses USB IP readiness", [](TestBoard& b) {
        b.cellular_network_ = false; b.ready = false;
        b.EnsureTfStartupQr(); Ready(b);
        Check(b.wait_calls == 0, "WiFi waited for USB IP");
    });
    Run("HTTP failures back off and recover", [](TestBoard& b) {
        b.network.responses.assign(9, Response{});
        for (size_t i = 0; i < 8; ++i) b.network.responses[i].status = 404;
        on_delay = [&]() { Check(b.display.diagnostic == "QR HTTP 404", "HTTP status missing on screen"); };
        b.EnsureTfStartupQr(); Ready(b, 9);
        Check(delays == std::vector<uint32_t>({2000,4000,8000,16000,32000,60000,60000,60000}), "retry backoff");
    });
    Run("HTTP open failure retried", [](TestBoard& b) {
        b.network.responses = {Response{}, Response{}};
        b.network.responses[0].open = false;
        b.EnsureTfStartupQr(); Ready(b, 2);
    });
    Run("partial body and size mismatch retried", [](TestBoard& b) {
        b.network.responses = {Response{}, Response{}, Response{}};
        b.network.responses[0].read_error_after = 3;
        b.network.responses[1].expected = 100;
        b.EnsureTfStartupQr(); Ready(b, 3);
    });
    Run("invalid download never replaces existing file", [](TestBoard& b) {
        Write(kPath, "old invalid JPEG");
        b.network.responses[0].body = "server error";
        b.network.responses[0].expected = 12;
        on_delay = [&]() {
            Check(Read(kPath) == "old invalid JPEG", "old file replaced before validation");
            Check(!fs::exists(kPartial), "failed download left staging bytes");
            b.network.responses.push_back(Response{}); on_delay = {};
        };
        b.EnsureTfStartupQr(); Ready(b, 2);
    });
    Run("unknown content length accepted only after JPEG validation", [](TestBoard& b) {
        b.network.responses[0].expected = 0;
        b.EnsureTfStartupQr(); Ready(b);
        Check(b.display.progress_total == 0 && b.display.progress_verified,
              "unknown length did not reach verification");
    });
    Run("oversized declared or streamed response rejected", [](TestBoard& b) {
        b.network.responses = {Response{}, Response{}, Response{}};
        b.network.responses[0].expected = 512 * 1024 + 1;
        b.network.responses[1].expected = 0;
        b.network.responses[1].body.assign(512 * 1024 + 1, 'x');
        b.EnsureTfStartupQr(); Ready(b, 3);
    });
    Run("decoder memory pressure preserves cache and avoids HTTP", [](TestBoard& b) {
        Write(kPath, Jpeg()); no_memory_decodes = 2;
        b.EnsureTfStartupQr(); Ready(b, 0);
        Check(delays == std::vector<uint32_t>({2000,4000}), "decode retry backoff");
    });
    Run("large local dimensions rejected before decoder allocation", [](TestBoard& b) {
        Write(kPath, Jpeg(65535, 65535));
        b.EnsureTfStartupQr(); Ready(b);
        Check(decode_calls == 1, "oversized cached JPEG reached decoder");
    });
    Run("large download dimensions rejected before decoder allocation", [](TestBoard& b) {
        b.network.responses = {Response{}, Response{}};
        b.network.responses[0].body = Jpeg(1025, 300);
        b.EnsureTfStartupQr(); Ready(b, 2);
        Check(decode_calls == 1, "oversized downloaded JPEG reached decoder");
    });
    Run("valid header with undecodable data is repaired", [](TestBoard& b) {
        auto corrupt = Jpeg(); corrupt.insert(corrupt.size() - 2, "corrupt scan");
        Write(kPath, corrupt);
        b.EnsureTfStartupQr(); Ready(b);
        Check(decode_calls == 3, "initial and post-network local checks must validate JPEG");
    });
    Run("downloaded JPEG survives decoder memory pressure without redownload", [](TestBoard& b) {
        no_memory_decodes = 2;
        on_delay = [&]() {
            Check(Read(kPartial) == Jpeg(), "completed transfer discarded on decoder OOM");
            Check(!fs::exists(kPath), "unvalidated transfer finalized on decoder OOM");
        };
        b.EnsureTfStartupQr(); Ready(b);
        Check(decode_calls == 3 && delays == std::vector<uint32_t>({2000,4000}), "staged decode retry");
        Check(!fs::exists(kPartial), "staged JPEG not finalized after memory recovery");
    });
    Run("directory at QR destination is a storage error", [](TestBoard& b) {
        fs::create_directory(kPath); b.EnsureTfStartupQr();
        Check(!b.storage_errors.empty() && b.network.requests == 0 && b.display.shows == 0, "directory accepted");
    });
    Run("cached IMEI and valid QR work before transport or live IMEI", [](TestBoard& b) {
        cached_imei = kImei; b.imei.clear(); b.startup_transport_started_ = false;
        Write(kPath, Jpeg()); b.EnsureTfStartupQr(); Ready(b, 0);
        Check(b.network_starts == 0 && delays.empty(), "local QR started networking or waited");
    });
    Run("cached IMEI with missing QR switches to live identity after network start", [](TestBoard& b) {
        cached_imei = "111111111111111";
        b.EnsureTfStartupQr(); Ready(b);
        Check(b.network_starts == 1, "missing local QR did not start network exactly once");
        Check(!fs::exists("sdcard/111111111111111.jpg"), "download used stale cached identity");
    });
    Run("WiFi waits for IP before downloading missing QR", [](TestBoard& b) {
        b.cellular_network_ = false; b.startup_network_ready_ = false;
        on_delay = [&]() { Check(b.network.requests == 0, "HTTP before WiFi ready"); b.startup_network_ready_ = true; };
        b.EnsureTfStartupQr(); Ready(b);
    });
    Run("directory at staging path is a storage error", [](TestBoard& b) {
        fs::create_directory(kPartial); b.EnsureTfStartupQr();
        Check(!b.storage_errors.empty() && b.display.shows == 0, "staging directory accepted");
    });
    for (int failure = 0; failure < 3; ++failure) {
        Run("write, fsync or rename failure never displays QR", [failure](TestBoard& b) {
            fail_write = failure == 0; fail_sync = failure == 1; fail_rename = failure == 2;
            b.EnsureTfStartupQr();
            Check(!b.storage_errors.empty() && b.display.shows == 0, "storage failure ignored");
            Check(!fs::exists(kPartial) && !fs::exists(kPath), "failed download committed");
            const char* operation = failure == 0 ? "WRITE" : (failure == 1 ? "SYNC" : "SAVE");
            const int code = failure == 0 ? ENOSPC : EIO;
            Check(b.display.diagnostic == std::string("QR TF ") + operation + " E" + std::to_string(code),
                  "TF failure stage or error number was lost");
        });
    }
    std::cout << tests << " TF QR worker scenarios passed\n";
}
