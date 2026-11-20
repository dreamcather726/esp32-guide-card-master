#ifndef JIUCHUAN_LOCATION_MQTT_CLIENT_H
#define JIUCHUAN_LOCATION_MQTT_CLIENT_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mqtt_client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <memory>
#include <functional>

class RndisBoard;
class SdContentManager;
class PositionSubscriber;

// Customer MQTT client. The historical class/file name is retained so the
// board integration stays small, but this no longer performs GNSS reporting.
class LocationMqttClient {
public:
    explicit LocationMqttClient(RndisBoard& board, std::function<bool()> network_ready);
    ~LocationMqttClient();

    LocationMqttClient(const LocationMqttClient&) = delete;
    LocationMqttClient& operator=(const LocationMqttClient&) = delete;

    void Start();
    void Stop();
    void ReplayContent(bool previous);
    void RequestOnlineReport() { report_requested_ = true; }

private:
    struct Credentials {
        std::string client_id;
        std::string username;
        std::string password;
        std::string pub_topic;
        std::string subscribe_topic;
        std::string wss_url;
        int expires_in = 0;
    };

    RndisBoard& board_;
    TaskHandle_t task_handle_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> connect_error_reported_{false};
    std::atomic<bool> report_requested_{false};
    std::atomic<bool> shutdown_scheduled_{false};
    int last_credentials_http_status_ = 0;
    std::mutex mqtt_mutex_;
    std::mutex state_mutex_;
    esp_mqtt_client_handle_t mqtt_client_ = nullptr;
    Credentials active_credentials_;
    std::unique_ptr<SdContentManager> content_manager_;
    std::unique_ptr<PositionSubscriber> position_subscriber_;

    std::string physical_imei_;
    std::string cab_guid_;
    std::string command_topic_;
    std::string event_topic_;
    std::string position_topic_;
    std::string position_compat_topic_;
    std::string group_topic_;
    std::string group_id_;
    std::string signal_strength_;
    std::string incoming_topic_;
    std::string incoming_payload_;
    bool incoming_retained_ = false;

    uint64_t runtime_base_seconds_ = 0;
    uint64_t user_playback_baseline_seconds_ = 0;
    uint32_t user_conversation_baseline_ = 0;

    static void TaskEntry(void* arg);
    static void MqttEventEntry(void* handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void* event_data);

    void Run();
    void SetScreenDiagnostic(const char* message);
    bool WaitOrStop(uint32_t delay_ms);
    bool FetchCredentials(const std::string& cab_guid,
                          Credentials& credentials);
    bool StartMqtt(const Credentials& credentials,
                   const std::string& physical_imei,
                   const std::string& cab_guid);
    void StopMqtt();
    void HandleMqttEvent(int32_t event_id, esp_mqtt_event_handle_t event);
    void HandleMqttPayload(const std::string& topic,
                           const std::string& payload,
                           bool retained);
    void SubscribeBusinessTopics(esp_mqtt_client_handle_t client);
    void UpdateGroupSubscription(const std::string& old_topic,
                                 const std::string& new_topic);
    void QuerySignalStrength();
    bool PublishOnlineReport();
    bool PublishCommandPayload(const std::string& payload);
    bool PublishExecution(const std::string& command_id,
                           bool success,
                           const std::string& message = "");
    void ScheduleNetworkReboot();
    bool SchedulePowerOff();
    void PersistRuntimeCheckpoint();
};

#endif  // JIUCHUAN_LOCATION_MQTT_CLIENT_H
