// The runner includes production methods verbatim; only network/timing are fake.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdint>
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

namespace fs = std::filesystem;
static std::vector<uint32_t> delays;
static std::vector<std::string> logs;
static bool fail_sync = false;
static std::function<void()> on_delay;
struct Application {
    int ready_notifications = 0;
    static Application& GetInstance() { static Application app; return app; }
    void NotifyTfStartAudioReady() { ++ready_notifications; }
};

static void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename... Args>
static void Log(const char*, const char* format, Args... args) {
    char buffer[1024];
    if constexpr (sizeof...(args) == 0) {
        std::snprintf(buffer, sizeof(buffer), "%s", format);
    } else {
        std::snprintf(buffer, sizeof(buffer), format, args...);
    }
    logs.emplace_back(buffer);
}

static void vTaskDelay(uint32_t delay) {
    delays.push_back(delay);
    if (on_delay) on_delay();
    Check(delays.size() < 100, "worker did not stop in test");
}

static int TestFsync(int fd) {
    if (fail_sync) {
        errno = EIO;
        return -1;
    }
    return ::fsync(fd);
}

static std::string Wav() {
    // PCM mono, 16 kHz, 16-bit, four bytes of audio.
    const unsigned char header[] = {
        'R','I','F','F', 40,0,0,0, 'W','A','V','E', 'f','m','t',' ',
        16,0,0,0, 1,0,1,0, 0x80,0x3e,0,0, 0,0x7d,0,0,
        2,0,16,0, 'd','a','t','a', 4,0,0,0, 0,0,0,0
    };
    return std::string(reinterpret_cast<const char*>(header), sizeof(header));
}

struct Response {
    std::string body = Wav();
    bool open = true;
    int status = 200;
    size_t expected = Wav().size();
    int read_error_after = -1;
};

struct FakeHttp {
    Response response;
    size_t offset = 0;
    void SetTimeout(int value) { Check(value == 15000, "HTTP timeout"); }
    bool Open(const char* method, const char* url) {
        Check(std::string(method) == "GET", "GET required");
        Check(std::string(url) == "https://iotossbj.suji.work/SDfile/TF/start.wav", "source URL");
        return response.open;
    }
    int GetStatusCode() const { return response.status; }
    size_t GetBodyLength() const { return response.expected; }
    int Read(char* buffer, size_t capacity) {
        if (response.read_error_after >= 0 && offset >= static_cast<size_t>(response.read_error_after)) {
            return -1;
        }
        size_t size = std::min(capacity, response.body.size() - offset);
        if (response.read_error_after >= 0) {
            size = std::min(size, static_cast<size_t>(response.read_error_after) - offset);
        }
        std::memcpy(buffer, response.body.data() + offset, size);
        offset += size;
        return static_cast<int>(size);
    }
    void Close() {}
};

struct FakeNetwork {
    std::vector<Response> responses{Response{}};
    size_t requests = 0;
    bool no_client = false;
    std::unique_ptr<FakeHttp> CreateHttp(int id) {
        Check(id == 3, "HTTP connection id");
        if (no_client) return nullptr;
        Check(requests < responses.size(), "unexpected extra HTTP request");
        auto result = std::make_unique<FakeHttp>();
        result->response = responses[requests++];
        return result;
    }
};

#include "../main/display/diagnostic_channel.h"
struct FakeDisplay {
    std::vector<std::string> notifications;
    void SetDiagnostic(DiagnosticChannel, const char*) {}
    void SetDiagnosticStatus(DiagnosticChannel, const char*) {}
    void ShowNotification(const char* text, int) { notifications.emplace_back(text); }
};

#define BOARD_TAG "TFTest"
#define SDCARD_MOUNT_POINT "sdcard"
#define ESP_LOGI Log
#define ESP_LOGW Log
#define ESP_LOGE Log
#define pdMS_TO_TICKS(ms) (ms)
#define fsync TestFsync
struct TestBoard {
    std::atomic<bool> startup_transport_started_{true}, startup_network_ready_{true};
    int audio_resolutions = 0;
    void ResolveStartupAudio() { ++audio_resolutions; }
    FakeNetwork network;
    FakeDisplay display;
    bool cellular_network_ = true;
    bool network_present = true;
    bool ready = true;
    int pending_waits = 0;
    int wait_calls = 0;
    FakeNetwork* GetNetwork() { return network_present ? &network : nullptr; }
    FakeDisplay* GetDisplay() { return &display; }
    bool IsNetworkReady() const { return ready; }
    bool WaitForNetworkReady(uint32_t timeout) {
        Check(timeout == 30000, "network wait timeout");
        ++wait_calls;
        if (pending_waits-- > 0) return false;
        ready = true;
        return true;
    }
#include "tf_audio_production.inc"
};
#undef fsync

static void Write(const fs::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary);
    output.write(content.data(), content.size());
    Check(output.good(), "test fixture write failed");
}

static std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

static void Ready(TestBoard& board, size_t requests = 1) {
    Check(Read("sdcard/start.wav") == Wav(), "final WAV bytes differ");
    Check(!fs::exists("sdcard/.start.wav.part"), "partial file left after success");
    Check(board.network.requests == requests, "unexpected request count");
    Check(board.display.notifications.empty(), "unexpected storage error notification");
    Check(Application::GetInstance().ready_notifications == 1, "validated file not announced exactly once");
    Check(board.audio_resolutions == 1, "startup flow not notified exactly once");
}

static int tests = 0;
static void Run(const char* name, const std::function<void(TestBoard&)>& test) {
    // The runner owns a unique temporary working directory, not a real TF card.
    fs::create_directory("case_" + std::to_string(++tests));
    const fs::path previous = fs::current_path();
    fs::current_path(previous / ("case_" + std::to_string(tests)));
    fs::create_directory("sdcard");
    delays.clear();
    logs.clear();
    fail_sync = false;
    on_delay = {};
    Application::GetInstance().ready_notifications = 0;
    TestBoard board;
    test(board);
    if (!board.display.notifications.empty()) {
        Check(Application::GetInstance().ready_notifications == 0, "storage error announced audio as ready");
    }
    fs::current_path(previous);
    std::cout << "PASS " << name << '\n';
}

int main() {
    Run("valid local audio does not wait for transport", [](TestBoard& b) {
        Write("sdcard/start.wav", Wav()); b.startup_transport_started_ = false;
        b.EnsureTfStartAudio(); Ready(b, 0);
        Check(delays.empty(), "local audio delayed by network startup");
    });
    Run("missing WiFi audio waits for transport and IP", [](TestBoard& b) {
        b.cellular_network_ = false;
        b.startup_transport_started_ = false; b.startup_network_ready_ = false;
        on_delay = [&]() {
            Check(b.network.requests == 0, "audio HTTP started before transport/IP");
            if (!b.startup_transport_started_) b.startup_transport_started_ = true;
            else b.startup_network_ready_ = true;
        };
        b.EnsureTfStartAudio(); Ready(b);
        Check(delays.size() == 2, "audio did not wait for both transport and WiFi IP");
    });
    Run("empty card", [](TestBoard& b) { b.EnsureTfStartAudio(); Ready(b); });
    Run("populated card preserves roles and hidden directories", [](TestBoard& b) {
        fs::create_directories("sdcard/9/role/sound");
        fs::create_directory("sdcard/System Volume Information");
        Write("sdcard/9/role/sound/001.wav", "existing audio");
        b.EnsureTfStartAudio();
        Ready(b);
        Check(Read("sdcard/9/role/sound/001.wav") == "existing audio", "other audio changed");
        Check(fs::is_directory("sdcard/System Volume Information"), "directory removed");
    });
    Run("leftover partial file does not block next boot", [](TestBoard& b) {
        Write("sdcard/.start.wav.part", "interrupted");
        b.EnsureTfStartAudio(); Ready(b);
    });
    Run("valid existing WAV avoids network", [](TestBoard& b) {
        Write("sdcard/start.wav", Wav());
        b.ready = false;
        b.EnsureTfStartAudio(); Ready(b, 0);
        Check(b.wait_calls == 0, "valid file waited for network");
    });
    Run("empty WAV repaired", [](TestBoard& b) {
        Write("sdcard/start.wav", ""); b.EnsureTfStartAudio(); Ready(b);
    });
    Run("short WAV repaired", [](TestBoard& b) {
        Write("sdcard/start.wav", Wav().substr(0, 12)); b.EnsureTfStartAudio(); Ready(b);
    });
    Run("bad WAV signature repaired", [](TestBoard& b) {
        Write("sdcard/start.wav", std::string(48, 'x')); b.EnsureTfStartAudio(); Ready(b);
    });
    Run("truncated RIFF payload repaired", [](TestBoard& b) {
        Write("sdcard/start.wav", Wav().substr(0, 44)); b.EnsureTfStartAudio(); Ready(b);
    });
    Run("RIFF unsigned length overflow rejected", [](TestBoard& b) {
        auto wav = Wav(); std::fill(wav.begin() + 4, wav.begin() + 8, '\xff');
        Write("sdcard/start.wav", wav); b.EnsureTfStartAudio(); Ready(b);
    });
    Run("network startup longer than old 15 attempts", [](TestBoard& b) {
        b.ready = false; b.pending_waits = 16;
        b.EnsureTfStartAudio(); Ready(b);
        Check(b.wait_calls == 17, "network waits exhausted download attempts");
    });
    Run("WiFi does not wait on USB readiness", [](TestBoard& b) {
        b.ready = false; b.cellular_network_ = false;
        b.EnsureTfStartAudio(); Ready(b);
        Check(b.wait_calls == 0, "WiFi waited on USB");
    });
    Run("HTTP open failure retried", [](TestBoard& b) {
        Response failure; failure.open = false;
        b.network.responses = {failure, Response{}};
        b.EnsureTfStartAudio(); Ready(b, 2);
        Check(delays == std::vector<uint32_t>{2000}, "initial backoff");
    });
    Run("HTTP failure has capped backoff and later recovers", [](TestBoard& b) {
        Response failure; failure.status = 503;
        b.network.responses.assign(18, failure); b.network.responses.emplace_back();
        b.EnsureTfStartAudio(); Ready(b, 19);
        Check(delays.size() == 18 && delays.back() == 60000, "capped retry delay");
        Check(*std::max_element(delays.begin(), delays.end()) == 60000, "retry overflow");
    });
    Run("unknown Content-Length with complete WAV", [](TestBoard& b) {
        b.network.responses[0].expected = 0;
        b.EnsureTfStartAudio(); Ready(b);
    });
    Run("interrupted transfer preserves old file until success", [](TestBoard& b) {
        Write("sdcard/start.wav", "old invalid file");
        b.network.responses[0].read_error_after = 20;
        std::string error;
        Check(b.DownloadInitialStartAudio(error) == TestBoard::TfDownloadResult::Retry, "read failure result");
        Check(Read("sdcard/start.wav") == "old invalid file", "old file removed too early");
        Check(!fs::exists("sdcard/.start.wav.part"), "partial file not cleaned");
        b.network.responses.emplace_back(); b.EnsureTfStartAudio(); Ready(b, 2);
    });
    Run("Content-Length mismatch retried", [](TestBoard& b) {
        b.network.responses[0].expected = 100;
        b.network.responses.emplace_back(); b.EnsureTfStartAudio(); Ready(b, 2);
    });
    Run("unknown length with incomplete WAV retried", [](TestBoard& b) {
        b.network.responses[0].expected = 0;
        b.network.responses[0].body.resize(44);
        b.network.responses.emplace_back(); b.EnsureTfStartAudio(); Ready(b, 2);
    });
    Run("HTML in a successful HTTP response rejected", [](TestBoard& b) {
        b.network.responses[0].body = std::string(48, 'x');
        b.network.responses.emplace_back(); b.EnsureTfStartAudio(); Ready(b, 2);
    });
    Run("empty HTTP response rejected", [](TestBoard& b) {
        b.network.responses[0].body.clear(); b.network.responses[0].expected = 0;
        b.network.responses.emplace_back(); b.EnsureTfStartAudio(); Ready(b, 2);
    });
    Run("directory named start.wav is preserved and reported", [](TestBoard& b) {
        fs::create_directory("sdcard/start.wav"); b.EnsureTfStartAudio();
        Check(fs::is_directory("sdcard/start.wav"), "directory removed");
        Check(b.network.requests == 0 && b.display.notifications.size() == 1, "path conflict not reported");
    });
    Run("temporary path write error reported", [](TestBoard& b) {
        fs::create_directory("sdcard/.start.wav.part"); b.EnsureTfStartAudio();
        Check(b.display.notifications.size() == 1 && delays.empty(), "storage error retried as network");
        Check(!fs::exists("sdcard/start.wav"), "invalid final file created");
    });
    Run("disk sync error cannot report success", [](TestBoard& b) {
        Write("sdcard/start.wav", "old invalid file"); fail_sync = true;
        b.EnsureTfStartAudio();
        Check(b.display.notifications.size() == 1, "sync error not reported");
        Check(Read("sdcard/start.wav") == "old invalid file", "file replaced despite sync error");
        Check(!fs::exists("sdcard/.start.wav.part"), "failed staging file left behind");
    });
    Run("absent network returns retry", [](TestBoard& b) {
        b.network_present = false; std::string error;
        Check(b.DownloadInitialStartAudio(error) == TestBoard::TfDownloadResult::Retry, "network missing result");
    });
    Run("HTTP allocation failure returns retry", [](TestBoard& b) {
        b.network.no_client = true; std::string error;
        Check(b.DownloadInitialStartAudio(error) == TestBoard::TfDownloadResult::Retry, "allocation result");
    });
    std::cout << tests << " TF audio scenarios passed\n";
}
