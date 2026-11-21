#include "sd_content_manager.h"

#include "application.h"
#include "board.h"
#include "display.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <http.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr const char* kTag = "SdContent";
constexpr const char* kSdRoot = "/sdcard";
constexpr const char* kHistoryPath = "/sdcard/player.list";
constexpr size_t kMaxHistoryEntries = 50;
constexpr uint64_t kSpaceReserveBytes = 1024 * 1024;

struct SyncTaskContext {
    SdContentManager* manager;
    SdContentManager::SyncRequest request;
};

std::string Trim(std::string value) {
    auto non_space = [](unsigned char ch) { return std::isspace(ch) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), non_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), non_space).base(),
                value.end());
    return value;
}

bool IsSafeSegment(const std::string& value) {
    return !value.empty() && value.size() <= 96 &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '_' || ch == '-';
        });
}

bool NormalizeRelativePath(const std::string& input, std::string& output) {
    output = Trim(input);
    std::replace(output.begin(), output.end(), '\\', '/');
    while (output.rfind("./", 0) == 0) {
        output.erase(0, 2);
    }
    // Customer file lists use one leading slash but still define paths
    // relative to the directory containing filelist.txt.
    if (!output.empty() && output.front() == '/') {
        output.erase(0, 1);
    }
    if (output.empty() || output.size() > 240 || output.front() == '/' ||
        output.find(':') != std::string::npos) {
        return false;
    }
    size_t start = 0;
    while (start <= output.size()) {
        const size_t end = output.find('/', start);
        const std::string segment = output.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

bool IsSafeFilename(const std::string& value) {
    std::string normalized;
    return NormalizeRelativePath(value, normalized) &&
           normalized.find('/') == std::string::npos;
}

bool NormalizeMp3Filename(const std::string& input, std::string& output,
                          bool accept_legacy_wav) {
    output = Trim(input);
    if (!IsSafeFilename(output)) {
        return false;
    }

    std::string lower = output;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    const auto ends_with = [&lower](const char* suffix) {
        const size_t length = std::strlen(suffix);
        return lower.size() >= length &&
            lower.compare(lower.size() - length, length, suffix) == 0;
    };

    if (output.find('.') == std::string::npos) {
        output += ".mp3";
        return true;
    }
    if (ends_with(".mp3")) {
        return true;
    }
    if (accept_legacy_wav && ends_with(".wav")) {
        output.replace(output.size() - 4, 4, ".mp3");
        return true;
    }
    return false;
}

bool GetFileSize(const std::string& path, uint64_t& size) {
    struct stat info = {};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        return false;
    }
    size = static_cast<uint64_t>(info.st_size);
    return true;
}

bool FileExists(const std::string& path) {
    uint64_t ignored = 0;
    return GetFileSize(path, ignored);
}

bool CheckSdCard(std::string& error) {
    struct stat info = {};
    if (stat(kSdRoot, &info) != 0 || !S_ISDIR(info.st_mode)) {
        error = "SD card not found or not mounted";
        return false;
    }

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(kSdRoot, &total_bytes, &free_bytes) != ESP_OK) {
        error = "SD card is not available";
        return false;
    }
    return true;
}

std::string ParentPath(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool EnsureDirectory(const std::string& path, std::string* error = nullptr) {
    if (path.empty()) {
        if (error) {
            *error = "empty directory path";
        }
        return false;
    }
    const auto fail = [error](const std::string& directory,
                              const char* operation, int error_number) {
        const std::string message = operation + std::string(" ") + directory +
            " failed: errno=" + std::to_string(error_number) + " (" +
            std::strerror(error_number) + ")";
        ESP_LOGE(kTag, "%s", message.c_str());
        if (error) {
            *error = message;
        }
        return false;
    };
    std::string current;
    size_t start = 0;
    if (path.front() == '/') {
        current = "/";
        start = 1;
    }
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string segment = path.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!segment.empty()) {
            if (current.size() > 1 && current.back() != '/') {
                current += '/';
            }
            current += segment;
            struct stat info = {};
            if (stat(current.c_str(), &info) != 0) {
                const int stat_error = errno;
                if (stat_error != ENOENT) {
                    return fail(current, "stat", stat_error);
                }
                errno = 0;
                if (mkdir(current.c_str(), 0775) != 0) {
                    const int mkdir_error = errno;
                    if (mkdir_error != EEXIST) {
                        return fail(current, "mkdir", mkdir_error);
                    }
                    if (stat(current.c_str(), &info) != 0 ||
                        !S_ISDIR(info.st_mode)) {
                        return fail(current, "verify directory", errno);
                    }
                } else {
                    ESP_LOGI(kTag, "Created SD directory: %s", current.c_str());
                }
            } else if (!S_ISDIR(info.st_mode)) {
                const std::string message = current + " exists but is not a directory";
                ESP_LOGE(kTag, "%s", message.c_str());
                if (error) {
                    *error = message;
                }
                return false;
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

bool RemoveTree(const std::string& path) {
    struct stat info = {};
    if (stat(path.c_str(), &info) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISDIR(info.st_mode)) {
        return remove(path.c_str()) == 0;
    }
    DIR* directory = opendir(path.c_str());
    if (!directory) {
        return false;
    }
    bool success = true;
    while (dirent* entry = readdir(directory)) {
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        success = RemoveTree(path + "/" + entry->d_name) && success;
    }
    closedir(directory);
    return rmdir(path.c_str()) == 0 && success;
}

std::string JoinUrl(const std::string& base, const std::string& path) {
    return base + (base.empty() || base.back() == '/' ? "" : "/") + path;
}

std::string JsonString(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    if (cJSON_IsNumber(item)) {
        return std::to_string(item->valueint);
    }
    return {};
}

void AddString(cJSON* root, const char* key, const std::string& value) {
    cJSON_AddStringToObject(root, key, value.c_str());
}

std::string SerializeJson(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    if (!text) {
        return {};
    }
    std::string result(text);
    cJSON_free(text);
    return result;
}

}  // namespace

SdContentManager::SdContentManager(PublishCallback publisher)
    : publisher_(std::move(publisher)) {
    if (xTaskCreate(PlaybackTaskEntry, "content_player", 8192, this, 3,
                    &playback_task_) != pdPASS) {
        playback_task_ = nullptr;
        ESP_LOGE(kTag, "Failed to create content playback task");
    }
}

SdContentManager::~SdContentManager() {
    stopping_ = true;
    if (playback_task_) {
        xTaskNotifyGive(playback_task_);
    }
    if (sync_task_) {
        xTaskNotifyGive(sync_task_);
    }
    for (int attempt = 0; attempt < 500 &&
         (playback_task_ || sync_task_); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool SdContentManager::StartSync(const SyncRequest& request) {
    if (sync_running_.exchange(true)) {
        return false;
    }
    auto* context = new (std::nothrow) SyncTaskContext{this, request};
    if (!context || xTaskCreate(SyncTaskEntry, "sd_sync", 12288, context, 3,
                                &sync_task_) != pdPASS) {
        delete context;
        sync_task_ = nullptr;
        sync_running_ = false;
        return false;
    }
    return true;
}

bool SdContentManager::QueuePlayback(const PlaybackRequest& request) {
    if (!playback_task_) {
        ESP_LOGE(kTag,
                 "[CONTENT_PLAY][QUEUE_REJECT] playback task is unavailable");
        return false;
    }
    size_t queue_size = 0;
    {
        std::lock_guard<std::mutex> lock(playback_mutex_);
        if (playback_jobs_.size() >= 16) {
            ESP_LOGE(kTag,
                     "[CONTENT_PLAY][QUEUE_REJECT] queue is full size=%u",
                     static_cast<unsigned>(playback_jobs_.size()));
            return false;
        }
        playback_jobs_.push_back({request, true});
        queue_size = playback_jobs_.size();
    }
    ESP_LOGI(kTag,
             "[CONTENT_PLAY][QUEUED] queue_size=%u player=%d group=%s role=%s scene=%s",
             static_cast<unsigned>(queue_size), request.player,
             request.group_id.empty() ? "<empty>" : request.group_id.c_str(),
             request.role_id.empty() ? "<empty>" : request.role_id.c_str(),
             request.scene_content_id.empty() ? "<empty>" : request.scene_content_id.c_str());
    xTaskNotifyGive(playback_task_);
    return true;
}

void SdContentManager::Replay(bool previous) {
    PlaybackRequest request;
    if (!GetReplayRequest(previous, request)) {
        Board::GetInstance().GetDisplay()->ShowNotification("无重复内容");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(playback_mutex_);
        if (playback_jobs_.size() >= 16) {
            Board::GetInstance().GetDisplay()->ShowNotification("Playback queue is full");
            return;
        }
        playback_jobs_.push_front({request, false});
    }
    Application::GetInstance().InterruptCurrentPlayback();
    xTaskNotifyGive(playback_task_);
}

void SdContentManager::SyncTaskEntry(void* arg) {
    std::unique_ptr<SyncTaskContext> context(static_cast<SyncTaskContext*>(arg));
    context->manager->HandleSync(context->request);
    context->manager->sync_running_ = false;
    context->manager->sync_task_ = nullptr;
    vTaskDelete(nullptr);
}

void SdContentManager::PlaybackTaskEntry(void* arg) {
    static_cast<SdContentManager*>(arg)->PlaybackLoop();
    static_cast<SdContentManager*>(arg)->playback_task_ = nullptr;
    vTaskDelete(nullptr);
}

void SdContentManager::PlaybackLoop() {
    while (!stopping_) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (!stopping_) {
            PlaybackJob job;
            {
                std::lock_guard<std::mutex> lock(playback_mutex_);
                if (playback_jobs_.empty()) {
                    break;
                }
                job = std::move(playback_jobs_.front());
                playback_jobs_.pop_front();
            }
            HandlePlayback(job);
        }
    }
}

bool SdContentManager::ParseFileList(
    const std::string& content, std::vector<FileEntry>& entries,
    std::string& error) const {
    size_t start = 0;
    while (start <= content.size()) {
        const size_t end = content.find('\n', start);
        std::string line = Trim(content.substr(
            start, end == std::string::npos ? std::string::npos : end - start));
        if (!line.empty() && line.front() != '#') {
            const size_t delimiter = line.find_last_of('|');
            if (delimiter == std::string::npos) {
                error = "filelist line must use path|size: " + line;
                return false;
            }
            std::string path;
            if (!NormalizeRelativePath(line.substr(0, delimiter), path)) {
                error = "unsafe filelist path: " + line.substr(0, delimiter);
                return false;
            }
            const std::string size_text = Trim(line.substr(delimiter + 1));
            char* size_end = nullptr;
            errno = 0;
            const unsigned long long size = std::strtoull(
                size_text.c_str(), &size_end, 10);
            if (errno != 0 || size_end == size_text.c_str() ||
                *size_end != '\0') {
                error = "invalid file size: " + size_text;
                return false;
            }
            entries.push_back({path, static_cast<uint64_t>(size)});
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    if (entries.empty()) {
        error = "filelist is empty";
        return false;
    }
    return true;
}

bool SdContentManager::DownloadFile(
    const std::string& url, const std::string& target,
    uint64_t expected_size, std::string& error) const {
    if (!CheckSdCard(error)) {
        ESP_LOGE(kTag, "[SD_SYNC][ERROR] %s", error.c_str());
        return false;
    }
    auto* network = Board::GetInstance().GetNetwork();
    if (!network) {
        error = "network is unavailable";
        return false;
    }
    auto http = network->CreateHttp(3);
    if (!http || !http->Open("GET", url)) {
        error = "HTTP open failed: " + url;
        return false;
    }
    const int status = http->GetStatusCode();
    if (status != 200) {
        error = status == 404
            ? "file not found: " + url
            : "HTTP " + std::to_string(status) + ": " + url;
        http->Close();
        return false;
    }
    const size_t content_length = http->GetBodyLength();
    if (content_length > 0 && content_length != expected_size) {
        error = "size header mismatch: " + url;
        http->Close();
        return false;
    }
    std::string directory_error;
    if (!EnsureDirectory(ParentPath(target), &directory_error)) {
        error = "SD: cannot create directory for " + target + ": " +
            directory_error;
        http->Close();
        return false;
    }
    const std::string temporary = target + ".part";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) {
        error = "SD: cannot open " + temporary;
        http->Close();
        return false;
    }
    uint64_t total = 0;
    char buffer[4096];
    bool success = true;
    while (!stopping_) {
        const int read = http->Read(buffer, sizeof(buffer));
        if (read < 0) {
            error = "HTTP read failed: " + url;
            success = false;
            break;
        }
        if (read == 0) {
            break;
        }
        if (std::fwrite(buffer, 1, read, file) != static_cast<size_t>(read)) {
            error = "SD: write failed for " + temporary;
            success = false;
            break;
        }
        total += static_cast<uint64_t>(read);
    }
    std::fflush(file);
    fsync(fileno(file));
    std::fclose(file);
    http->Close();
    if (stopping_) {
        error = "download stopped";
        success = false;
    }
    if (success && total != expected_size) {
        error = "downloaded size mismatch for " + url;
        success = false;
    }
    if (!success) {
        remove(temporary.c_str());
        return false;
    }
    remove(target.c_str());
    if (rename(temporary.c_str(), target.c_str()) != 0) {
        remove(temporary.c_str());
        error = "SD: cannot finalize " + target;
        return false;
    }
    return true;
}

void SdContentManager::HandleSync(const SyncRequest& request) {
    if (!IsSafeSegment(request.group_id) ||
        (request.mode != "increment" && request.mode != "complete") ||
        (request.filelist_url.rfind("http://", 0) != 0 &&
         request.filelist_url.rfind("https://", 0) != 0)) {
        PublishSyncStatus("error", request, 0, "invalid sync request");
        return;
    }

    std::string sd_error;
    if (!CheckSdCard(sd_error)) {
        ESP_LOGE(kTag, "[SD_SYNC][ERROR] %s", sd_error.c_str());
        PublishSyncStatus("error", request, 0, sd_error);
        return;
    }

    auto* network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(3) : nullptr;
    if (!http || !http->Open("GET", request.filelist_url)) {
        PublishSyncStatus("error", request, 0, "filelist download failed");
        return;
    }
    const int status = http->GetStatusCode();
    const std::string filelist = status == 200 ? http->ReadAll() : std::string();
    http->Close();
    if (status != 200 || filelist.empty()) {
        PublishSyncStatus("error", request, 0,
                          "filelist HTTP " + std::to_string(status));
        return;
    }

    std::vector<FileEntry> entries;
    std::string error;
    if (!ParseFileList(filelist, entries, error)) {
        PublishSyncStatus("error", request, 0, error);
        return;
    }

    const std::string group_prefix = request.group_id + "/";
    for (auto& entry : entries) {
        if (entry.relative_path.rfind(group_prefix, 0) == 0) {
            entry.relative_path.erase(0, group_prefix.size());
        }
        if (entry.relative_path.empty()) {
            PublishSyncStatus("error", request, 0, "invalid empty file path");
            return;
        }
    }

    const std::string final_root = std::string(kSdRoot) + "/" + request.group_id;
    const std::string staging_root = std::string(kSdRoot) + "/.sync_" + request.group_id;
    const std::string target_root = request.mode == "complete"
        ? staging_root : final_root;

    uint64_t required_bytes = 0;
    for (const auto& entry : entries) {
        uint64_t existing_size = 0;
        const bool valid_existing = request.mode == "increment" &&
            GetFileSize(final_root + "/" + entry.relative_path, existing_size) &&
            existing_size == entry.size_bytes;
        if (!valid_existing) {
            required_bytes += entry.size_bytes;
        }
    }
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(kSdRoot, &total_bytes, &free_bytes) != ESP_OK) {
        PublishSyncStatus("error", request, 0, "SD capacity read failed");
        return;
    }
    if (required_bytes + kSpaceReserveBytes > free_bytes) {
        PublishSyncStatus("error", request, 0, "SD free space is insufficient");
        return;
    }

    std::string directory_error;
    if (request.mode == "complete") {
        if (!RemoveTree(staging_root) ||
            !EnsureDirectory(staging_root, &directory_error)) {
            PublishSyncStatus("error", request, 0,
                              "cannot prepare complete-update directory: " +
                              directory_error);
            return;
        }
    } else if (!EnsureDirectory(final_root, &directory_error)) {
        PublishSyncStatus("error", request, 0,
                          "cannot create group directory: " + directory_error);
        return;
    }

    for (const auto& entry : entries) {
        const std::string directory = ParentPath(
            target_root + "/" + entry.relative_path);
        directory_error.clear();
        if (!EnsureDirectory(directory, &directory_error)) {
            if (request.mode == "complete") {
                RemoveTree(staging_root);
            }
            PublishSyncStatus(
                "error", request, 0,
                "cannot prepare SD content directory: " + directory_error);
            return;
        }
    }
    ESP_LOGI(kTag, "Prepared SD directory tree for %u files under %s",
             static_cast<unsigned>(entries.size()), target_root.c_str());

    const size_t slash = request.filelist_url.find_last_of('/');
    const std::string url_root = slash == std::string::npos
        ? request.filelist_url : request.filelist_url.substr(0, slash);
    size_t file_failures = 0;
    for (size_t index = 0; index < entries.size() && !stopping_; ++index) {
        const auto& entry = entries[index];
        const std::string target = target_root + "/" + entry.relative_path;
        uint64_t existing_size = 0;
        const bool skip = request.mode == "increment" &&
            GetFileSize(target, existing_size) && existing_size == entry.size_bytes;
        if (!skip) {
            error.clear();
            if (!DownloadFile(JoinUrl(url_root, entry.relative_path), target,
                              entry.size_bytes, error)) {
                ++file_failures;
                const int progress = static_cast<int>((index + 1) * 100 / entries.size());
                PublishSyncStatus("error", request, progress, error);
                if (error.rfind("file not found:", 0) != 0) {
                    if (request.mode == "complete") {
                        RemoveTree(staging_root);
                    }
                    return;
                }
            }
        }
        if ((index + 1) % 20 == 0 || index + 1 == entries.size()) {
            const int progress = static_cast<int>((index + 1) * 100 / entries.size());
            PublishSyncStatus("Downloading", request, progress);
        }
    }
    if (stopping_) {
        if (request.mode == "complete") {
            RemoveTree(staging_root);
        }
        return;
    }

    if (request.mode == "complete") {
        const std::string backup_root = std::string(kSdRoot) + "/.old_" + request.group_id;
        RemoveTree(backup_root);
        const bool had_old = rename(final_root.c_str(), backup_root.c_str()) == 0;
        if (rename(staging_root.c_str(), final_root.c_str()) != 0) {
            if (had_old) {
                rename(backup_root.c_str(), final_root.c_str());
            }
            PublishSyncStatus("error", request, 100,
                              "cannot activate complete update");
            return;
        }
        if (had_old) {
            RemoveTree(backup_root);
        }
    }

    PublishSyncStatus("done", request, 100,
                      file_failures == 0 ? "" :
                      std::to_string(file_failures) + " files were not found");
    Application::GetInstance().NotifySdContentSynchronized(request.group_id);
}

void SdContentManager::HandlePlayback(const PlaybackJob& job) {
    const auto& request = job.request;
    ESP_LOGI(kTag,
             "[CONTENT_PLAY][HANDLE] player=%d cmdID=%s group=%s role=%s scene=%s",
             request.player,
             request.command_id.empty() ? "<none>" : request.command_id.c_str(),
             request.group_id.empty() ? "<empty>" : request.group_id.c_str(),
             request.role_id.empty() ? "<empty>" : request.role_id.c_str(),
             request.scene_content_id.empty() ? "<empty>" : request.scene_content_id.c_str());

    std::string sd_error;
    if (!CheckSdCard(sd_error)) {
        ESP_LOGE(kTag, "[CONTENT_PLAY][ERROR] %s", sd_error.c_str());
        PublishExecution(request.command_id, false, sd_error);
        return;
    }

    if (request.player != 1) {
        ESP_LOGW(kTag,
                 "[CONTENT_PLAY][REJECT] unsupported player=%d", request.player);
        PublishExecution(request.command_id, false,
                         "player=2 m3u8 is not configured");
        return;
    }
    if (!IsSafeSegment(request.group_id) || !IsSafeSegment(request.role_id)) {
        ESP_LOGW(kTag,
                 "[CONTENT_PLAY][REJECT] invalid group or role group=%s role=%s",
                 request.group_id.empty() ? "<empty>" : request.group_id.c_str(),
                 request.role_id.empty() ? "<empty>" : request.role_id.c_str());
        PublishExecution(request.command_id, false, "invalid playback identifiers");
        return;
    }

    const std::string requested_scene_filename =
        Trim(request.scene_content_id);
    std::string scene_filename;
    if (!NormalizeMp3Filename(requested_scene_filename, scene_filename, true)) {
        ESP_LOGW(kTag,
                 "[CONTENT_PLAY][REJECT] scene audio must be MP3 filename=%s",
                 requested_scene_filename.empty() ? "<empty>" :
                 requested_scene_filename.c_str());
        PublishExecution(request.command_id, false,
                         "scene audio must be an mp3 file");
        return;
    }
    if (scene_filename != requested_scene_filename) {
        ESP_LOGI(kTag,
                 "[CONTENT_PLAY][MP3_MAP] requested=%s resolved=%s",
                 requested_scene_filename.c_str(), scene_filename.c_str());
    }

    const std::string role_root = request.group_id + "/" + request.role_id;
    const std::string main_file = role_root + "/sound/" + scene_filename;
    const std::string main_path = std::string(kSdRoot) + "/" + main_file;
    if (!FileExists(main_path)) {
        ESP_LOGW(kTag,
                 "[CONTENT_PLAY][MISSING] main audio path=%s",
                 main_path.c_str());
        PublishExecution(request.command_id, false,
                         "main audio not found: " + main_file);
        return;
    }

    auto resolve_effect = [&](const char* type, const std::string& filename) {
        if (filename.empty()) {
            return std::string();
        }
        std::string normalized_filename;
        if (!NormalizeMp3Filename(filename, normalized_filename, false)) {
            if (!filename.empty()) {
                ESP_LOGW(kTag,
                         "[CONTENT_PLAY][OPTIONAL_REJECT] type=%s filename=%s reason=MP3 required",
                         type, filename.c_str());
            }
            return std::string();
        }
        const std::string role_effect =
            role_root + "/effect/" + normalized_filename;
        if (FileExists(std::string(kSdRoot) + "/" + role_effect)) {
            return role_effect;
        }
        const std::string group_effect =
            request.group_id + "/effect/" + normalized_filename;
        if (FileExists(std::string(kSdRoot) + "/" + group_effect)) {
            return group_effect;
        }
        ESP_LOGW(kTag,
                 "[CONTENT_PLAY][OPTIONAL_MISSING] type=%s role_path=%s/%s group_path=%s/%s",
                 type, kSdRoot, role_effect.c_str(), kSdRoot,
                 group_effect.c_str());
        return std::string();
    };

    std::vector<std::string> files;
    const std::string pre = resolve_effect("pre", request.pre_sound);
    const std::string post = resolve_effect("post", request.post_sound);
    if (!request.pre_sound.empty() && pre.empty()) {
        const std::string message =
            "pre_sound file not found or unsupported: " + request.pre_sound;
        ESP_LOGE(kTag, "[CONTENT_PLAY][ERROR] %s", message.c_str());
        PublishExecution(request.command_id, false, message);
        return;
    }
    if (!request.post_sound.empty() && post.empty()) {
        const std::string message =
            "post_sound file not found or unsupported: " + request.post_sound;
        ESP_LOGE(kTag, "[CONTENT_PLAY][ERROR] %s", message.c_str());
        PublishExecution(request.command_id, false, message);
        return;
    }
    if (!pre.empty()) {
        files.push_back(pre);
    }
    files.push_back(main_file);
    if (!post.empty()) {
        files.push_back(post);
    }

    std::string image_file;
    if (!request.picture.empty() && !IsSafeFilename(request.picture)) {
        const std::string message =
            "picture file path is invalid: " + request.picture;
        ESP_LOGE(kTag, "[CONTENT_PLAY][ERROR] %s", message.c_str());
        PublishExecution(request.command_id, false, message);
        return;
    }
    if (!request.picture.empty()) {
        const std::string candidate = role_root + "/picture/" + request.picture;
        if (FileExists(std::string(kSdRoot) + "/" + candidate)) {
            image_file = candidate;
        } else {
            const std::string message =
                "picture file not found: " + candidate;
            ESP_LOGE(kTag, "[CONTENT_PLAY][ERROR] %s", message.c_str());
            PublishExecution(request.command_id, false, message);
            return;
        }
    }

    std::string role_name = request.role_name;
    auto& app = Application::GetInstance();
    if (role_name.empty() && app.GetCurrentRoleId() == request.role_id) {
        role_name = app.GetCurrentRoleName();
    }
    if (role_name.empty()) {
        role_name = request.role_id;
    }
    const std::string footer = role_name + " " + request.scene_content_id;

    if (job.record_history) {
        AppendHistory(request);
    }
    ESP_LOGI(kTag,
             "[CONTENT_PLAY][DISPATCH] files=%u main=%s pre=%s post=%s picture=%s",
             static_cast<unsigned>(files.size()), main_path.c_str(),
             pre.empty() ? "<none>" : pre.c_str(),
             post.empty() ? "<none>" : post.c_str(),
             image_file.empty() ? "<none>" : image_file.c_str());
    app.PlaySdAudioSequence(
        files, request.text, image_file, footer,
        [this, command_id = request.command_id](bool success) {
            PublishExecution(command_id, success,
                             success ? "" : "audio playback failed or was interrupted");
        }, GuidePlayback{GuideAudioKind::Narration, role_name, request.text, ""});
}

void SdContentManager::PublishSyncStatus(
    const char* status, const SyncRequest& request, int progress,
    const std::string& message) const {
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    AddString(root, "downSDfile", status);
    AddString(root, "cmdID", request.command_id);
    AddString(root, "progress", std::to_string(std::clamp(progress, 0, 100)) + "%");
    AddString(root, "groupID", request.group_id);
    if (std::strcmp(status, "done") != 0) {
        AddString(root, "filelist", request.filelist_url);
    }
    if (!message.empty()) {
        AddString(root, "msg", message);
    }
    const std::string payload = SerializeJson(root);
    cJSON_Delete(root);
    if (!payload.empty() && publisher_) {
        publisher_(payload);
    }
}

void SdContentManager::PublishExecution(
    const std::string& command_id, bool success,
    const std::string& message) const {
    if (command_id.empty()) {
        ESP_LOGW(kTag,
                 "[CONTENT_PLAY][RESULT_WITHOUT_CMDID] success=%d message=%s",
                 success, message.empty() ? "<none>" : message.c_str());
    }
    if (!publisher_) {
        ESP_LOGW(kTag,
                 "[CONTENT_PLAY][RESULT_UNPUBLISHED] publisher unavailable cmdID=%s",
                 command_id.c_str());
        return;
    }
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    AddString(root, "deviceExecute", success ? "OK" : "error");
    AddString(root, "cmdID", command_id);
    if (!message.empty()) {
        AddString(root, "msg", message);
    }
    const std::string payload = SerializeJson(root);
    cJSON_Delete(root);
    if (!payload.empty()) {
        publisher_(payload);
    }
}

void SdContentManager::LoadHistory() {
    if (history_loaded_) {
        return;
    }
    history_loaded_ = true;
    std::ifstream file(kHistoryPath);
    std::string line;
    while (std::getline(file, line)) {
        cJSON* root = cJSON_Parse(line.c_str());
        if (!root) {
            continue;
        }
        PlaybackRequest request;
        request.player = 1;
        request.group_id = JsonString(root, "groupID");
        request.role_id = JsonString(root, "roleID");
        request.role_name = JsonString(root, "roleName");
        request.scene_content_id = JsonString(root, "scene_contentID");
        request.pre_sound = JsonString(root, "pre_sound");
        request.post_sound = JsonString(root, "post_sound");
        request.text = JsonString(root, "txt");
        request.picture = JsonString(root, "picture");
        cJSON_Delete(root);
        if (IsSafeSegment(request.group_id) && IsSafeSegment(request.role_id) &&
            IsSafeFilename(request.scene_content_id)) {
            history_.push_back(std::move(request));
        }
    }
    if (history_.size() > kMaxHistoryEntries) {
        history_.erase(history_.begin(),
                       history_.end() - kMaxHistoryEntries);
    }
}

void SdContentManager::SaveHistory() {
    const std::string temporary = std::string(kHistoryPath) + ".part";
    std::ofstream file(temporary, std::ios::trunc);
    if (!file) {
        ESP_LOGW(kTag, "Cannot write playback history");
        return;
    }
    for (const auto& request : history_) {
        cJSON* root = cJSON_CreateObject();
        AddString(root, "groupID", request.group_id);
        AddString(root, "roleID", request.role_id);
        AddString(root, "roleName", request.role_name);
        AddString(root, "scene_contentID", request.scene_content_id);
        AddString(root, "pre_sound", request.pre_sound);
        AddString(root, "post_sound", request.post_sound);
        AddString(root, "txt", request.text);
        AddString(root, "picture", request.picture);
        file << SerializeJson(root) << '\n';
        cJSON_Delete(root);
    }
    file.flush();
    file.close();
    remove(kHistoryPath);
    if (rename(temporary.c_str(), kHistoryPath) != 0) {
        ESP_LOGW(kTag, "Cannot finalize playback history");
        remove(temporary.c_str());
    }
}

void SdContentManager::AppendHistory(const PlaybackRequest& request) {
    std::lock_guard<std::mutex> lock(history_mutex_);
    LoadHistory();
    history_.push_back(request);
    if (history_.size() > kMaxHistoryEntries) {
        history_.erase(history_.begin());
    }
    replay_cursor_ = static_cast<int>(history_.size()) - 1;
    SaveHistory();
}

bool SdContentManager::GetReplayRequest(
    bool previous, PlaybackRequest& request) {
    std::lock_guard<std::mutex> lock(history_mutex_);
    LoadHistory();
    if (history_.empty()) {
        return false;
    }
    if (previous) {
        if (replay_cursor_ < 0 || replay_cursor_ >= static_cast<int>(history_.size())) {
            replay_cursor_ = static_cast<int>(history_.size()) - 1;
        }
        replay_cursor_ = std::max(0, replay_cursor_ - 1);
    } else {
        replay_cursor_ = static_cast<int>(history_.size()) - 1;
    }
    request = history_[replay_cursor_];
    request.command_id.clear();
    return true;
}
