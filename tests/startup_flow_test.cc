#include "../main/boards/jiuchuan-s3/startup_flow.h"
#include <cassert>
#include <iostream>

int main() {
    {
        startup::Flow flow;
        auto action = flow.AudioResolved();
        assert(!action.start_network && !action.start_business);
        action = flow.QrPresentationFinished();
        assert(action.start_network && action.start_business);
        action = flow.QrPresentationFinished();
        assert(!action.start_network && !action.start_business);
        action = flow.AudioResolved();
        assert(!action.start_network && !action.start_business);
    }
    std::cout << "PASS cached QR/audio: network starts only after display finishes, once\n";
    {
        startup::Flow flow;
        auto action = flow.QrPresentationFinished();
        assert(action.start_network && !action.start_business);
        action = flow.AudioResolved();
        assert(!action.start_network && action.start_business);
    }
    std::cout << "PASS cached QR, missing audio: network after display, business after audio download\n";
    for (bool audio_first : {false, true}) {
        startup::Flow flow;
        if (audio_first) {
            const auto action = flow.AudioResolved();
            assert(!action.start_network && !action.start_business);
        }
        auto action = flow.LocalQrUnavailable();
        assert(action.start_network && !action.start_business);
        action = flow.LocalQrUnavailable();
        assert(!action.start_network && !action.start_business);
        if (!audio_first) {
            action = flow.AudioResolved();
            assert(!action.start_network && !action.start_business);
        }
        action = flow.QrPresentationFinished();
        assert(!action.start_network && action.start_business);
    }
    std::cout << "PASS missing QR: start transport, preserve independently ready audio, wait full presentation\n";
    {
        startup::Flow flow;
        auto action = flow.LocalQrUnavailable();
        assert(action.start_network && !action.start_business);
        action = flow.QrPresentationFinished();
        assert(!action.start_network && !action.start_business);
        action = flow.AudioResolved();
        assert(!action.start_network && action.start_business);
    }
    std::cout << "PASS both missing: QR/audio may complete in either order without premature business startup\n";
}
