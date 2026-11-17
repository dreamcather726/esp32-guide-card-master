// Production Application queue and presentation methods, fake RTOS/I/O only.
#include "../main/display/guide_state.h"
#include "../main/boards/common/user_binding.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <deque>
#include <functional>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <sys/stat.h>
#include <vector>

#define CONFIG_BOARD_TYPE_JIUCHUAN 1
template<class... Args> void Log(Args&&...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGE(...) Log(__VA_ARGS__)
#define TAG "test"
static constexpr const char* kSdCardMountPoint = "sd";
static constexpr size_t kMaxSdAudioQueueSize = 32;
using BaseType_t = int;
static constexpr int pdPASS = 1, kAbortReasonNone = 0;
enum ListeningMode { kListeningModeAutoStop, kListeningModeManualStop };
enum class PowerSaveLevel { PERFORMANCE, LOW_POWER };
static int64_t esp_timer_get_time() { static int64_t time = 0; return time += 100000; }
static std::deque<std::function<void()>> workers;
static bool fail_create = false;
static int xTaskCreate(void (*entry)(void*), const char*, int, void* arg, int, void*) {
    if (fail_create) return 0;
    workers.push_back([=]() { entry(arg); }); return pdPASS;
}
static void vTaskDelete(void*) {}
static bool NormalizeSdAudioFilename(const std::string& name, std::string& output) {
    if (name.empty() || name.front() == '/' || name.find("..") != std::string::npos) return false;
    output = name; return true;
}
struct LvglImage {};
struct FakeDisplay {
    GuideView view;
    std::string text, footer;
    int notifications = 0;
    void SetGuideView(const GuideView& value) { view = value; }
    void SetPreviewImage(std::unique_ptr<LvglImage>) {}
    void SetContentFooter(const char* value) { footer = value; }
    void SetChatMessage(const char*, const char* value) { text = value; }
    void ShowNotification(const char*) { ++notifications; }
};
struct Board {
    FakeDisplay display;
    UserBindingSnapshot binding;
    bool pending = false;
    static Board& GetInstance() { static Board board; return board; }
    FakeDisplay* GetDisplay() { return &display; }
    bool IsStartupPresentationPending() { return pending; }
    bool RequiresUserBinding() { return true; }
    UserBindingSnapshot GetUserBinding() { return binding; }
    void SetPowerSaveLevel(PowerSaveLevel) {}
};
struct FakeAudio {
    bool idle = true, success = true;
    uint32_t generation = 0;
    bool IsIdle() { return idle; }
    void ResetDecoder() {}
    void CancelFilePlayback() { ++generation; }
    uint32_t GetFilePlaybackGeneration() { return generation; }
    bool PlayFile(const std::string&) { return success; }
    void WaitForPlaybackIdle() {}
};
struct FakeProtocol { void SendStopListening() {} };
class Application {
public:
#include "queue_types.inc"
    static Application& GetInstance() { static Application app; return app; }
    DeviceState state = kDeviceStateIdle;
    GuidePlayback active_guide_playback_;
    bool guide_local_playback_active_ = false;
    std::string guide_speech_text_;
    std::string role_name = "文庙";
    std::atomic<bool> sd_audio_playing_{false}, sd_audio_pending_{false};
    std::deque<SdAudioQueueItem> sd_audio_queue_;
    std::deque<std::function<void()>> tasks;
    FakeAudio audio_service_;
    std::unique_ptr<FakeProtocol> protocol_ = std::make_unique<FakeProtocol>();
    bool tts_stop_pending_ = false, aborted_ = false, return_idle_after_current_tts_ = false;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    DeviceState GetDeviceState() { return state; }
    bool SetDeviceState(DeviceState next) { state = next; return true; }
    void AbortSpeaking(int) { aborted_ = true; }
    void RecordSdAudioPlaybackDuration(uint64_t) {}
    std::string GetCurrentRoleName() { return role_name; }
    void Schedule(std::function<void()>&& fn) { tasks.push_back(std::move(fn)); }
    FakeAudio& GetAudioService() { return audio_service_; }
    std::unique_ptr<LvglImage> LoadSdPreviewImage(const std::string&) { return nullptr; }
    void PlaySdAudioSequence(const std::vector<std::string>&, const std::string&, const std::string&,
        const std::string&, std::function<void(bool)> = {}, const GuidePlayback& = {});
    void CancelSdAudioPlaybackOnMainTask();
    void StartNextSdAudioPlayback();
    void UpdateGuideDisplay();
    void FinishTtsWhenIdle();
    void Drain() {
        while (!tasks.empty()) { auto fn = std::move(tasks.front()); tasks.pop_front(); fn(); }
        FinishTtsWhenIdle(); UpdateGuideDisplay();
    }
    void FinishOne() {
        assert(!workers.empty()); auto fn = std::move(workers.front()); workers.pop_front(); fn(); Drain();
    }
};
#include "guide_runtime.inc"
int main() {
    mkdir("sd", 0700);
    for (auto* name : {"safety.mp3", "pre.mp3", "main.mp3", "post.mp3"}) std::ofstream(std::string("sd/") + name).put('x');
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    app.UpdateGuideDisplay();
    assert(board.display.view.page == GuidePage::Idle && board.display.view.hint == "请关联当前用户");
    int completed = 0;
    app.PlaySdAudioSequence({"safety.mp3"}, "安全提示", "", "", [&](bool success) { assert(success); ++completed; },
                           {GuideAudioKind::Safety, "", "", ""});
    app.Drain();
    assert(board.display.view.page == GuidePage::Safety && completed == 0);
    app.FinishOne();
    assert(board.display.view.page == GuidePage::Idle && completed == 1);
    app.PlaySdAudioSequence({"pre.mp3", "main.mp3", "post.mp3"}, "多段讲解内容", "", "旧字幕",
        [&](bool success) { assert(success); ++completed; }, {GuideAudioKind::Narration, "斜梁先生", "", ""});
    app.Drain();
    for (int i = 0; i < 3; ++i) {
        assert(board.display.view.page == GuidePage::Narration && board.display.view.title == "斜梁先生");
        assert(board.display.view.text == "多段讲解内容");
        app.FinishOne();
    }
    assert(completed == 2 && board.display.view.page == GuidePage::Idle && board.display.footer.empty());
    app.PlaySdAudioSequence({"main.mp3"}, "会被中断", "", "", [&](bool success) { assert(!success); ++completed; });
    app.Drain(); app.CancelSdAudioPlaybackOnMainTask(); app.state = kDeviceStateListening; app.Drain();
    assert(board.display.view.page == GuidePage::Listening);
    app.FinishOne();
    assert(board.display.view.page == GuidePage::Listening && completed == 3);
    app.state = kDeviceStateIdle;
    app.PlaySdAudioSequence({"absent.mp3"}, "不能残留", "", "", [&](bool success) { assert(!success); ++completed; });
    app.Drain(); assert(board.display.view.page == GuidePage::Idle && completed == 4);
    fail_create = true;
    app.PlaySdAudioSequence({"main.mp3"}, "不能卡住", "", "", [&](bool success) { assert(!success); ++completed; });
    app.Drain(); fail_create = false;
    assert(board.display.view.page == GuidePage::Idle && completed == 5 && workers.empty());
    board.binding.version = 1; board.binding.user_guid = "test-user"; board.binding.token = "test-token";
    board.binding.usage_session_id = "test-session"; board.binding.state = BindingState::Pending;
    app.Drain(); assert(board.display.view.hint == "用户验证中");
    board.binding.state = BindingState::Ready; app.Drain(); assert(board.display.view.hint.empty());
    board.binding.state = BindingState::Error; app.Drain(); assert(board.display.view.hint == "用户验证失败");
    board.binding = {}; app.Drain(); assert(board.display.view.hint == "请关联当前用户");
    app.state = kDeviceStateSpeaking; app.audio_service_.idle = false; app.tts_stop_pending_ = true;
    app.guide_speech_text_ = "最后一段还没播放完"; app.aborted_ = false;
    app.Drain(); assert(board.display.view.page == GuidePage::Narration);
    app.audio_service_.idle = true; app.Drain();
    assert(board.display.view.page == GuidePage::Listening);
    app.state = kDeviceStateSpeaking; app.tts_stop_pending_ = true; app.return_idle_after_current_tts_ = true;
    app.Drain(); assert(board.display.view.page == GuidePage::Idle);
    std::cout << "PASS production Application SD queue, three-part playback, cancellation, missing files, task failure, verified binding hints and TTS speaker drain\n";
}
