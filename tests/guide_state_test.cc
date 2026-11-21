#include "../main/display/guide_state.h"
#include <cassert>
#include <iostream>

int main() {
    GuideInput input;
    assert(ResolveGuideView(input).page == GuidePage::Legacy);
    input.state = kDeviceStateIdle;
    assert(ResolveGuideView(input).page == GuidePage::Legacy); // QR owns screen.
    input.startup_pending = false;
    auto view = ResolveGuideView(input);
    assert(view.page == GuidePage::Idle && view.number == "--:--" && view.hint == "请关联当前用户");
    input.clock = "08:43";
    input.binding = GuideBinding::Pending;
    assert(ResolveGuideView(input).hint == "用户验证中");
    input.binding = GuideBinding::Error;
    assert(ResolveGuideView(input).hint == "用户验证失败");
    input.binding = GuideBinding::Ready;
    view = ResolveGuideView(input);
    assert(view.hint.empty() && view.number == "08:43");
    input.local_playback = true;
    input.playback.kind = GuideAudioKind::Safety;
    view = ResolveGuideView(input);
    assert(view.page == GuidePage::Safety && view.title == "安全提示" && view.speaker);
    input.state = kDeviceStateActivating;
    assert(ResolveGuideView(input).page == GuidePage::Safety); // Group may arrive before activation task finishes.
    input.startup_pending = true;
    assert(ResolveGuideView(input).page == GuidePage::Legacy);
    input.startup_pending = false;
    input.state = kDeviceStateIdle;
    input.playback = {GuideAudioKind::Narration, "斜梁先生", "完整讲解文本", "24812"};
    view = ResolveGuideView(input);
    assert(view.page == GuidePage::Narration && view.title == "斜梁先生" && view.text == "完整讲解文本");
    // AI idle/close events cannot replace a still-playing local sequence.
    input.state = kDeviceStateConnecting;
    assert(ResolveGuideView(input) == view);
    input.local_playback = false;
    assert(ResolveGuideView(input).page == GuidePage::Connecting);
    input.state = kDeviceStateListening;
    view = ResolveGuideView(input);
    assert(view.page == GuidePage::Listening && view.text == "请说话" && !view.speaker);
    input.speech_text = "刚才识别的文字";
    assert(ResolveGuideView(input).text == "刚才识别的文字");
    input.state = kDeviceStateSpeaking;
    input.role_name = "文庙";
    input.speech_text = "讲解的下一句话";
    view = ResolveGuideView(input);
    assert(view.page == GuidePage::Narration && view.title == "文庙" && view.speaker);
    input.state = kDeviceStateIdle;
    view = ResolveGuideView(input);
    assert(view.page == GuidePage::Idle && view.text.empty() && !view.speaker);
    input.binding = GuideBinding::Unbound;
    assert(ResolveGuideView(input).hint == "请关联当前用户");
    for (auto state : {kDeviceStateStarting, kDeviceStateActivating, kDeviceStateUpgrading,
                       kDeviceStateWifiConfiguring, kDeviceStateFatalError}) {
        input.state = state;
        assert(ResolveGuideView(input).page == GuidePage::Legacy);
    }
    assert(GuideClock(0, kGuideDefaultTimezoneMinutes) == "--:--");
    assert(GuideClock(1704069780, kGuideDefaultTimezoneMinutes) == "08:43");
    assert(GuideClock(1704124740, kGuideDefaultTimezoneMinutes) == "23:59");
    assert(GuideClock(1704124800, kGuideDefaultTimezoneMinutes) == "00:00");
    assert(GuideClock(1704069780, -60) == "23:43");
    std::cout << "PASS guide startup priority, clock validity/rollover, binding lifecycle, SD safety/narration, listening/TTS and idle recovery\n";
}
