#pragma once

namespace startup {

struct Actions {
    bool start_network = false;
    bool start_business = false;
};

// Called only on the application task. Resource workers submit events rather
// than starting networking or business services on their own threads.
class Flow {
public:
    Actions LocalQrUnavailable() { return Advance(true); }
    Actions QrPresentationFinished() {
        qr_finished_ = true;
        return Advance(true);
    }
    Actions AudioResolved() {
        audio_resolved_ = true;
        return Advance(false);
    }

private:
    bool network_requested_ = false;
    bool qr_finished_ = false;
    bool audio_resolved_ = false;
    bool business_started_ = false;

    Actions Advance(bool request_network) {
        Actions actions;
        if (request_network && !network_requested_) {
            network_requested_ = true;
            actions.start_network = true;
        }
        if (network_requested_ && qr_finished_ && audio_resolved_ && !business_started_) {
            business_started_ = true;
            actions.start_business = true;
        }
        return actions;
    }
};

}  // namespace startup
