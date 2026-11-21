#ifndef JIUCHUAN_SD_CONTENT_MANAGER_H
#define JIUCHUAN_SD_CONTENT_MANAGER_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class SdContentManager {
public:
    struct SyncRequest {
        std::string mode;
        std::string command_id;
        std::string group_id;
        std::string filelist_url;
    };

    struct PlaybackRequest {
        int player = 1;
        std::string command_id;
        std::string group_id;
        std::string role_id;
        std::string role_name;
        std::string scene_content_id;
        std::string pre_sound;
        std::string post_sound;
        std::string text;
        std::string picture;
    };

    using PublishCallback = std::function<void(const std::string& payload)>;

    explicit SdContentManager(PublishCallback publisher);
    ~SdContentManager();

    bool StartSync(const SyncRequest& request);
    bool QueuePlayback(const PlaybackRequest& request);
    void Replay(bool previous);

private:
    struct FileEntry {
        std::string relative_path;
        uint64_t size_bytes = 0;
    };

    struct PlaybackJob {
        PlaybackRequest request;
        bool record_history = true;
    };

    PublishCallback publisher_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> sync_running_{false};
    TaskHandle_t sync_task_ = nullptr;
    TaskHandle_t playback_task_ = nullptr;
    std::mutex playback_mutex_;
    std::deque<PlaybackJob> playback_jobs_;
    std::mutex history_mutex_;
    std::vector<PlaybackRequest> history_;
    bool history_loaded_ = false;
    int replay_cursor_ = -1;

    static void SyncTaskEntry(void* arg);
    static void PlaybackTaskEntry(void* arg);

    void PlaybackLoop();
    void HandleSync(const SyncRequest& request);
    void HandlePlayback(const PlaybackJob& job);
    bool ParseFileList(const std::string& content,
                       std::vector<FileEntry>& entries,
                       std::string& error) const;
    bool DownloadFile(const std::string& url,
                      const std::string& target,
                      uint64_t expected_size,
                      std::string& error) const;
    void PublishSyncStatus(const char* status,
                           const SyncRequest& request,
                           int progress,
                           const std::string& message = "") const;
    void PublishExecution(const std::string& command_id,
                          bool success,
                          const std::string& message = "") const;

    void LoadHistory();
    void SaveHistory();
    void AppendHistory(const PlaybackRequest& request);
    bool GetReplayRequest(bool previous, PlaybackRequest& request);
};

#endif  // JIUCHUAN_SD_CONTENT_MANAGER_H
