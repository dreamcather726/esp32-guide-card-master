#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <cstdint>
#include <functional>
#include <vector>
#include <atomic>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "user_binding.h"
#include "display/guide_state.h"

class LvglImage;

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
#define MAIN_EVENT_CUSTOMER_MQTT_ONLINE (1 << 13)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    /** Switch to the next role/voice binding configured by the manager. */
    void SwitchRoleVoiceBinding();

    /** Select one configured role/voice binding and report the server result. */
    void SelectRoleVoiceBinding(const std::string& role_id,
                                std::function<void(bool)> completion,
                                bool play_introduction = true);

    /** Handle the first valid content group received from customer MQTT. */
    void HandleContentGroupId(const std::string& group_id);
    void NotifySdContentSynchronized(const std::string& group_id);
    void NotifyCustomerMqttOnline();

    /** Queue an audio file stored below /sdcard for local playback. */
    void PlaySdAudioFile(const std::string& filename);
    void NotifyTfStartAudioReady();
    void NotifyStartupPresentationDone();
    void PlaySdAudioSequence(const std::vector<std::string>& filenames,
                             const std::string& display_text,
                             const std::string& image_filename,
                             const std::string& footer,
                             std::function<void(bool)> completion = {},
                             const GuidePlayback& presentation = {});
    void InterruptCurrentPlayback();

    /** Runtime counters used by the customer MQTT online report. */
    uint64_t GetSdAudioPlaybackDurationSeconds() const;
    uint32_t GetConversationCount() const;
    std::string GetCurrentRoleId() const;
    std::string GetCurrentRoleName() const;

    /** Stop protocol and audio work before the board enters deep sleep. */
    void PrepareForShutdown();
    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void ApplyUserBinding(const UserBindingCommand& command,
        std::function<void(BindingCommandResult, const std::string&)> completion);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    
    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

private:
    bool CanStartPersonalChat();
    void VerifyUserBinding();
    void ScheduleAudioChannelTask(std::function<void()>&& callback);
    std::atomic<uint32_t> audio_channel_epoch_{0};
    std::atomic<bool> firmware_upgrade_in_progress_{false};
    struct SdAudioSequenceState {
        bool success = true;
        std::function<void(bool)> completion;
        GuidePlayback presentation;
    };

    struct SdAudioQueueItem {
        std::string filename;
        std::string display_text;
        std::string image_filename;
        std::string footer;
        std::shared_ptr<SdAudioSequenceState> sequence;
        bool first = false;
        bool last = false;
    };

    Application();
    ~Application();

    std::mutex mutex_;
    mutable std::mutex statistics_mutex_;
    mutable std::mutex role_mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    std::unique_ptr<Ota> ota_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool startup_network_connected_pending_ = false;
    bool deferred_ota_check_needed_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    bool role_switch_response_pending_ = false;
    bool role_switch_scan_active_ = false;
    bool role_switch_restore_pending_ = false;
    bool role_switch_play_introduction_ = true;
    bool return_idle_after_current_tts_ = false;
    bool tts_stop_pending_ = false;
    GuidePlayback active_guide_playback_;
    bool guide_local_playback_active_ = false;
    std::string guide_speech_text_;
    std::atomic<bool> sd_audio_playing_{false};
    std::atomic<bool> sd_audio_pending_{false};
    bool tf_start_audio_ready_ = false;
    bool tf_start_audio_started_ = false;
    std::atomic<bool> initial_group_announcement_requested_{false};
    std::atomic<bool> initial_group_announcement_started_{false};
    std::atomic<bool> initial_group_announcement_completed_{false};
    std::deque<SdAudioQueueItem> sd_audio_queue_;
    int64_t role_switch_request_time_us_ = 0;
    uint64_t sd_audio_playback_duration_ms_ = 0;
    uint32_t conversation_count_ = 0;
    std::string current_role_id_;
    std::string current_role_name_;
    std::string content_group_id_;
    std::string requested_role_id_;
    std::string role_switch_origin_role_id_;
    std::string role_switch_origin_role_name_;
    std::string role_switch_first_role_id_;
    std::string role_switch_first_role_name_;
    std::vector<std::string> role_switch_seen_role_ids_;
    size_t role_switch_binding_count_ = 0;
    std::function<void(bool)> role_switch_completion_;
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;
    TaskHandle_t deferred_ota_task_handle_ = nullptr;


    // Event handlers
    void HandleStateChangedEvent();
    void UpdateGuideDisplay();
    void FinishTtsWhenIdle();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void TryStartTfStartAudio();
    void HandleWakeWordDetectedEvent();

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ScheduleDeferredOtaCheck();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    void StartNextSdAudioPlayback();
    void RecordSdAudioPlaybackDuration(uint64_t duration_ms);
    void RecordConversation();
    void SetCurrentRole(const std::string& role_id, const std::string& role_name);
    void CompleteRoleSwitch(bool success);
    bool ShouldValidateRoleIntroduction() const;
    bool RoleIntroductionFileExists(const std::string& role_id,
                                    std::string* relative_path = nullptr) const;
    void ResetRoleSwitchScan();
    void SendNextRoleSwitchCandidate();
    void SendRoleSwitchRestore(const std::string& role_id);
    void TryStartInitialGroupAnnouncement();
    void PlayCurrentRoleIntroduction();
    void CancelSdAudioPlaybackOnMainTask();
    std::unique_ptr<LvglImage> LoadSdPreviewImage(
        const std::string& filename) const;
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
