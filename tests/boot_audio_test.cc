// The production queue types and methods are included verbatim by the runner.
#include <atomic>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include "../main/display/guide_state.h"

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

#define ESP_LOGI(...) do {} while (false)
#define ESP_LOGW(...) do {} while (false)

struct FakeAudio {
    bool idle = true;
    bool IsIdle() const { return idle; }
};

struct Application {
#include "sd_queue_types.inc"
    DeviceState state = kDeviceStateStarting;
    bool tf_start_audio_ready_ = false;
    bool tf_start_audio_started_ = false;
    std::atomic<bool> sd_audio_playing_{false};
    std::atomic<bool> sd_audio_pending_{false};
    std::deque<SdAudioQueueItem> sd_audio_queue_;
    std::deque<std::function<void()>> tasks;
    FakeAudio audio_service_;
    unsigned starts = 0;

    DeviceState GetDeviceState() const { return state; }
    void Schedule(std::function<void()> task) { tasks.push_back(std::move(task)); }
    void StartNextSdAudioPlayback() { ++starts; }
    void NotifyTfStartAudioReady();
    void TryStartTfStartAudio();
    void Drain() {
        while (!tasks.empty()) {
            auto task = std::move(tasks.front()); tasks.pop_front(); task();
        }
    }
    void Finish(bool success) {
        auto item = std::move(sd_audio_queue_.front()); sd_audio_queue_.pop_front();
        item.sequence->completion(success);
        sd_audio_pending_.store(false);
        sd_audio_playing_.store(false);
    }
};

#include "boot_audio_production.inc"

static int tests = 0;
static void Run(const char* name, const std::function<void(Application&)>& test) {
    Application app;
    test(app);
    ++tests;
    std::cout << "PASS " << name << '\n';
}

int main() {
    Run("no playback without validated file", [](Application& a) {
        a.TryStartTfStartAudio(); Check(a.starts == 0, "unvalidated playback");
    });
    Run("worker notification executes on main task", [](Application& a) {
        a.NotifyTfStartAudioReady();
        Check(a.starts == 0 && !a.tf_start_audio_ready_, "worker mutated main state");
        a.Drain();
        Check(a.starts == 1 && a.sd_audio_pending_, "main task did not start playback");
        const auto& item = a.sd_audio_queue_.front();
        Check(item.filename == "start.wav" && item.first && item.last, "wrong startup item");
        Check(item.display_text.empty() && item.footer.empty(), "filename exposed on screen");
        Check(a.state == kDeviceStateStarting, "startup changed network state");
    });
    Run("duplicate readiness notification does not replay", [](Application& a) {
        a.NotifyTfStartAudioReady(); a.NotifyTfStartAudioReady(); a.Drain();
        Check(a.starts == 1 && a.sd_audio_queue_.size() == 1, "duplicate startup");
        a.Finish(true); a.NotifyTfStartAudioReady(); a.Drain(); a.TryStartTfStartAudio();
        Check(a.starts == 1 && a.sd_audio_queue_.empty(), "replayed after completion");
    });
    Run("cancel or failure is not automatically retried", [](Application& a) {
        a.NotifyTfStartAudioReady(); a.Drain(); a.Finish(false);
        a.TryStartTfStartAudio(); a.NotifyTfStartAudioReady(); a.Drain();
        Check(a.starts == 1, "user cancellation replayed");
    });
    for (auto state : {kDeviceStateListening, kDeviceStateSpeaking, kDeviceStateConnecting,
                       kDeviceStateActivating, kDeviceStateUpgrading, kDeviceStateWifiConfiguring,
                       kDeviceStateAudioTesting, kDeviceStateUnknown}) {
        Run("busy state defers until idle", [state](Application& a) {
            a.state = state; a.NotifyTfStartAudioReady(); a.Drain();
            Check(a.starts == 0 && a.state == state, "interrupted current activity");
            a.state = kDeviceStateIdle; a.TryStartTfStartAudio();
            Check(a.starts == 1, "did not resume at idle");
        });
    }
    Run("safety reminder already playing is not interrupted", [](Application& a) {
        a.sd_audio_playing_ = true; a.NotifyTfStartAudioReady(); a.Drain();
        Check(a.starts == 0, "interrupted existing SD playback");
        a.sd_audio_playing_ = false; a.TryStartTfStartAudio();
        Check(a.starts == 1, "not started after SD playback");
    });
    Run("pending role announcement remains first", [](Application& a) {
        a.sd_audio_pending_ = true; a.NotifyTfStartAudioReady(); a.Drain();
        Check(a.starts == 0, "pending SD playback preempted");
        a.sd_audio_pending_ = false; a.TryStartTfStartAudio();
        Check(a.starts == 1, "not started after pending audio");
    });
    Run("queued content is preserved", [](Application& a) {
        Application::SdAudioQueueItem existing; existing.filename = "9/effect/SafetyReminder.mp3";
        a.sd_audio_queue_.push_back(std::move(existing));
        a.NotifyTfStartAudioReady(); a.Drain();
        Check(a.starts == 0 && a.sd_audio_queue_.size() == 1, "queued content replaced");
        a.sd_audio_queue_.clear(); a.TryStartTfStartAudio();
        Check(a.starts == 1, "startup not retried after queue drained");
    });
    Run("TTS output must finish even when UI is idle", [](Application& a) {
        a.state = kDeviceStateIdle; a.audio_service_.idle = false;
        a.NotifyTfStartAudioReady(); a.Drain();
        Check(a.starts == 0, "TTS output interrupted");
        a.audio_service_.idle = true; a.TryStartTfStartAudio();
        Check(a.starts == 1, "did not start after TTS drained");
    });
    Run("new boot can play again", [](Application& a) {
        a.NotifyTfStartAudioReady(); a.Drain(); a.Finish(true);
        Application next; next.NotifyTfStartAudioReady(); next.Drain();
        Check(next.starts == 1, "next boot suppressed");
    });
    std::cout << tests << " boot audio scenarios passed\n";
}
