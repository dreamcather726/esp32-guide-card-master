#pragma once
#include "../device_state.h"
#include <string>
#include <ctime>

enum class GuidePage { Legacy, Idle, Connecting, Safety, Narration, Listening };
enum class GuideBinding { NotRequired, Unbound, Pending, Ready, Error };
enum class GuideAudioKind { Startup, Safety, Narration };

struct GuidePlayback {
    GuideAudioKind kind = GuideAudioKind::Narration;
    std::string title;
    std::string text;
    std::string scene;
};

struct GuideView {
    GuidePage page = GuidePage::Legacy;
    std::string title;
    std::string text;
    std::string number;
    std::string hint;
    bool speaker = false;
    bool operator==(const GuideView& other) const {
        return page == other.page && title == other.title && text == other.text &&
            number == other.number && hint == other.hint && speaker == other.speaker;
    }
};

struct GuideInput {
    DeviceState state = kDeviceStateStarting;
    bool startup_pending = true;
    bool local_playback = false;
    GuidePlayback playback;
    GuideBinding binding = GuideBinding::Unbound;
    std::string role_name;
    std::string speech_text;
    std::string clock = "--:--";
};

// The OTA service already applies its timezone offset before settimeofday.
// Format that clock directly; never add a second UTC+8 offset or show boot time.
inline std::string GuideClock(std::time_t now) {
    if (now < 1704067200) return "--:--";
    std::tm local{};
    if (!gmtime_r(&now, &local)) return "--:--";
    char text[6];
    if (std::strftime(text, sizeof(text), "%H:%M", &local) != 5) return "--:--";
    return text;
}

// Resolve from real activity, not translated status strings. Local SD playback
// deliberately does not change the AI state or reset the active audio decoder.
inline GuideView ResolveGuideView(const GuideInput& input) {
    GuideView view;
    if (input.startup_pending || input.state == kDeviceStateUpgrading ||
        input.state == kDeviceStateWifiConfiguring || input.state == kDeviceStateAudioTesting ||
        input.state == kDeviceStateFatalError) return view;
    if (input.local_playback && input.playback.kind != GuideAudioKind::Startup) {
        view.speaker = true;
        if (input.playback.kind == GuideAudioKind::Safety) {
            view.page = GuidePage::Safety;
            view.title = "安全提示";
            view.text = "播放：安全提示";
        } else {
            view.page = GuidePage::Narration;
            view.title = input.playback.title.empty() ? input.role_name : input.playback.title;
            if (view.title.empty()) view.title = "讲解中";
            view.text = input.playback.text;
            view.number = input.playback.scene;
        }
    } else if (input.state == kDeviceStateStarting || input.state == kDeviceStateActivating ||
               input.state == kDeviceStateUnknown) {
        return view;
    } else if (input.state == kDeviceStateListening) {
        view.page = GuidePage::Listening;
        view.title = "聆听中…";
        view.text = input.speech_text.empty() ? "请说话" : input.speech_text;
    } else if (input.state == kDeviceStateSpeaking) {
        view.page = GuidePage::Narration;
        view.speaker = true;
        view.title = input.role_name.empty() ? "讲解中" : input.role_name;
        view.text = input.speech_text.empty() ? "正在讲解…" : input.speech_text;
    } else if (input.state == kDeviceStateConnecting) {
        view.page = GuidePage::Connecting;
        view.title = "正在连接…";
        view.text = "请稍候";
    } else {
        view.page = GuidePage::Idle;
        view.title = "待命";
        view.number = input.clock;
        switch (input.binding) {
            case GuideBinding::Unbound: view.hint = "请关联当前用户"; break;
            case GuideBinding::Pending: view.hint = "用户验证中"; break;
            case GuideBinding::Error: view.hint = "用户验证失败"; break;
            default: break;
        }
    }
    return view;
}
