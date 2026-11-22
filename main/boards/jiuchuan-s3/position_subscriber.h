#pragma once
#include "position_data.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <mqtt_client.h>

class RndisBoard;

// Receives ML307C telemetry independently of the customer WSS credentials.
// It never publishes position data or subscribes to another device's IMEI.
class PositionSubscriber {
public:
    PositionSubscriber(RndisBoard& board, std::function<bool()> network_ready);
    ~PositionSubscriber();
    // Called by the existing cabinet worker; no separate management task.
    void Poll();
private:
    RndisBoard& board_;
    std::function<bool()> network_ready_;
    std::atomic<bool> stopping_{false};
    int64_t next_start_ = 0;
    int64_t received_at_ = 0;  // Protected by navigation_mutex_, including expiry.
    int64_t first_report_deadline_ = 0;
    std::mutex navigation_mutex_;
    esp_mqtt_client_handle_t client_ = nullptr;
    bool mqtt_connected_ = false;
    std::string imei_, topic_, client_id_;
    position::Message message_;
    bool StartClient(const std::string& imei);
    void StopClient();
    void ClearPosition();
    void SetScreenDiagnostic(const char* message, bool fault = true);
    static void Event(void*, esp_event_base_t, int32_t, void*);
    void HandleEvent(int32_t event_id, esp_mqtt_event_handle_t event);
};
