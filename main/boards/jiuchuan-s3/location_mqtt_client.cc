#include "location_mqtt_client.h"

#include "application.h"
#include "audio_codec.h"
#include "backlight.h"
#include "config.h"
#include "display.h"
#include "rndis_board.h"
#include "settings.h"
#include "sd_content_manager.h"
#include "system_info.h"
#include "user_binding_json.h"
#include "position_subscriber.h"
#include "position_mqtt_config.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_vfs_fat.h>
#include <lwip/sockets.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace {

constexpr const char* kTag = "CabinetMqtt";
// Log only scheme/host/port; omit URL credentials, paths and query tokens.
std::string BrokerAddressForLog(const std::string& uri) {
    const auto scheme = uri.find("://");
    const auto start = scheme == std::string::npos ? 0 : scheme + 3;
    auto authority = uri.substr(start, uri.find_first_of("/?#", start) - start);
    const auto at = authority.rfind('@');
    if (at != std::string::npos) authority.erase(0, at + 1);
    for (char& ch : authority) if (static_cast<unsigned char>(ch) < 0x20) ch = '?';
    return (scheme == std::string::npos ? "" : uri.substr(0, start)) + authority;
}
constexpr const char* kBusinessBrokerUri = JIUCHUAN_BUSINESS_MQTT_URI;
constexpr const char* kSettingsNamespace = "cab_mqtt";
constexpr const char* kGroupTopicPrefix = "topic/group/";
// One worker services both clients after the existing startup gate.
constexpr uint32_t kRetryDelayMs = 2000;
constexpr uint32_t kConnectWaitMs = 30000;
constexpr uint32_t kReconnectGraceMs = 60000;
constexpr uint32_t kOnlineReportIntervalSeconds = 60;

struct BleBeaconSample {
    char mac[18]{};
    int rssi = 0;
    int64_t seen_us = 0;
};

constexpr int64_t kBleBeaconMaxAgeUs = 90LL * 1000 * 1000;
constexpr uint16_t kBlueScanPort = 8000;
constexpr int64_t kBlueScanIntervalUs = 10LL * 1000 * 1000;
constexpr int64_t kBlueScanAckTimeoutUs = 3LL * 1000 * 1000;
std::mutex ble_beacon_mutex;
std::array<BleBeaconSample, 16> ble_beacons{};

std::unique_ptr<cJSON, decltype(&cJSON_Delete)> BuildBleBeaconList(const char* mac_key) {
    const auto snapshot = [] {
        std::lock_guard lock(ble_beacon_mutex);
        return ble_beacons;
    }();
    const int64_t now = esp_timer_get_time();
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> items(cJSON_CreateArray(), cJSON_Delete);
    if (!items) return items;
    for (const auto& sample : snapshot) {
        if (sample.mac[0] == '\0' || now - sample.seen_us >= kBleBeaconMaxAgeUs) continue;
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> entry(cJSON_CreateObject(), cJSON_Delete);
        if (!entry || !cJSON_AddStringToObject(entry.get(), mac_key, sample.mac) ||
            !cJSON_AddNumberToObject(entry.get(), "rssi", sample.rssi) ||
            !cJSON_AddItemToArray(items.get(), entry.get())) {
            items.reset();
            return items;
        }
        entry.release();
    }
    return items;
}

int AddBleBeaconReport(cJSON* root) {
    auto items = BuildBleBeaconList("macAddress");
    if (!items) return -1;
    const int count = cJSON_GetArraySize(items.get());
    if (!cJSON_AddItemToObject(root, "bleBeacons", items.get())) return -1;
    items.release();
    return count;
}

std::string Trim(std::string value) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(),
                             [&](unsigned char ch) { return !is_space(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [&](unsigned char ch) { return !is_space(ch); })
                    .base(),
                value.end());
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

bool IsEmptyCommandValue(const std::string& input) {
    std::string value = Trim(input);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value.empty() || value == "null" || value == "none";
}

int HexNibble(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

bool ParseRgbColor(const char* value, IndexedRgbColor& pixel) {
    if (!value) {
        return false;
    }
    const std::string color(value);
    if (color.size() != 7 || color.front() != '#') {
        return false;
    }
    int components[6] = {};
    for (size_t i = 0; i < 6; ++i) {
        components[i] = HexNibble(color[i + 1]);
        if (components[i] < 0) {
            return false;
        }
    }
    pixel.red = static_cast<uint8_t>((components[0] << 4) | components[1]);
    pixel.green = static_cast<uint8_t>((components[2] << 4) | components[3]);
    pixel.blue = static_cast<uint8_t>((components[4] << 4) | components[5]);
    return true;
}

bool ParseRgbField(cJSON* root, const char* key, size_t led_count,
                   size_t max_active, bool& requested,
                   std::vector<IndexedRgbColor>& pixels,
                   std::string& error) {
    requested = false;
    pixels.clear();
    cJSON* field = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!field || cJSON_IsNull(field)) {
        return true;
    }
    if (cJSON_IsString(field) &&
        IsEmptyCommandValue(field->valuestring ? field->valuestring : "")) {
        return true;
    }

    requested = true;
    if (!cJSON_IsArray(field)) {
        error = std::string(key) +
            " must be an array of {index,color} objects";
        return false;
    }
    const int item_count = cJSON_GetArraySize(field);
    if (item_count < 0 || static_cast<size_t>(item_count) > led_count) {
        error = std::string(key) + " contains too many entries";
        return false;
    }

    std::vector<bool> seen(led_count + 1, false);
    size_t active_count = 0;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, field) {
        if (!cJSON_IsObject(item)) {
            error = std::string(key) + " entries must be objects";
            return false;
        }
        cJSON* index = cJSON_GetObjectItemCaseSensitive(item, "index");
        cJSON* color = cJSON_GetObjectItemCaseSensitive(item, "color");
        if (!cJSON_IsNumber(index) ||
            index->valuedouble != static_cast<double>(index->valueint) ||
            index->valueint < 1 ||
            static_cast<size_t>(index->valueint) > led_count) {
            error = std::string(key) + " index must be an integer from 1 to " +
                std::to_string(led_count);
            return false;
        }
        if (seen[index->valueint]) {
            error = std::string(key) + " contains duplicate LED index " +
                std::to_string(index->valueint);
            return false;
        }
        if (!cJSON_IsString(color) || !color->valuestring) {
            error = std::string(key) + " color must use #RRGGBB";
            return false;
        }

        IndexedRgbColor pixel;
        pixel.index = static_cast<uint16_t>(index->valueint);
        if (!ParseRgbColor(color->valuestring, pixel)) {
            error = std::string(key) + " color must use #RRGGBB";
            return false;
        }
        seen[pixel.index] = true;
        if (pixel.red != 0 || pixel.green != 0 || pixel.blue != 0) {
            ++active_count;
            if (active_count > max_active) {
                error = std::string(key) + " supports at most " +
                    std::to_string(max_active) + " active LEDs";
                return false;
            }
        }
        pixels.push_back(pixel);
    }
    return true;
}

std::string CommandPayloadForLog(const cJSON* root) {
    cJSON* copy = cJSON_Duplicate(root, true);
    if (!copy) {
        return "<unable-to-copy-json>";
    }
    if (!RedactSensitiveJsonFields(copy)) {
        cJSON_Delete(copy);
        return "<redaction-failed>";
    }
    char* text = cJSON_PrintUnformatted(copy);
    cJSON_Delete(copy);
    if (!text) {
        return "<unable-to-format-json>";
    }
    std::string result(text);
    cJSON_free(text);
    return result;
}

std::string JsonScalar(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    if (cJSON_IsNumber(item)) {
        char value[32] = {};
        if (item->valuedouble == static_cast<double>(item->valueint)) {
            snprintf(value, sizeof(value), "%d", item->valueint);
        } else {
            snprintf(value, sizeof(value), "%.6g", item->valuedouble);
        }
        return value;
    }
    return {};
}

bool TryFirstJsonScalar(cJSON* object,
                        std::initializer_list<const char*> keys,
                        std::string& value) {
    for (const char* key : keys) {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
        if (!item) {
            continue;
        }
        if (cJSON_IsString(item)) {
            value = item->valuestring ? item->valuestring : "";
            return true;
        }
        if (cJSON_IsNumber(item)) {
            value = JsonScalar(object, key);
            return true;
        }
        return false;
    }
    return false;
}

std::string NormalizeAudioFile(const char* raw_value) {
    if (!raw_value) {
        return {};
    }
    std::string value = Trim(raw_value);
    std::replace(value.begin(), value.end(), '\\', '/');
    const size_t slash = value.find_last_of('/');
    if (slash != std::string::npos) {
        value = value.substr(slash + 1);
    }
    if (value.empty() || value == "." || value == "..") {
        return {};
    }

    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    if (lower == "null" || lower == "none" || lower == "nil" ||
        lower == "false" || lower == "0" || lower == "!null" ||
        lower == "lnull") {
        return {};
    }
    const bool supported =
        (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".wav") == 0) ||
        (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".ogg") == 0);
    if (!supported || value.size() > 128) {
        return {};
    }
    for (unsigned char ch : value) {
        if (!std::isalnum(ch) && ch != '.' && ch != '_' && ch != '-') {
            return {};
        }
    }
    return value;
}

std::string ParseCsq(const std::string& response) {
    constexpr const char* kPrefix = "+CSQ:";
    size_t position = response.find(kPrefix);
    if (position == std::string::npos) {
        return {};
    }
    position += std::char_traits<char>::length(kPrefix);
    size_t end = response.find_first_of("\r\n", position);
    std::string value = Trim(response.substr(position, end - position));
    const size_t comma = value.find(',');
    if (comma == std::string::npos) {
        return {};
    }
    const std::string rssi = Trim(value.substr(0, comma));
    const std::string ber = Trim(value.substr(comma + 1));
    auto digits_only = [](const std::string& text) {
        return !text.empty() &&
               std::all_of(text.begin(), text.end(), [](unsigned char ch) {
                   return std::isdigit(ch) != 0;
               });
    };
    if (!digits_only(rssi) || !digits_only(ber)) {
        return {};
    }
    return rssi + "," + ber;
}

void GetSdCardSpaceKb(uint64_t& capacity_kb, uint64_t& remaining_kb) {
    capacity_kb = 0;
    remaining_kb = 0;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes) != ESP_OK) {
        return;
    }
    capacity_kb = total_bytes / 1024;
    remaining_kb = free_bytes / 1024;
}

void AddString(cJSON* root, const char* key, const std::string& value) {
    cJSON_AddStringToObject(root, key, value.c_str());
}

}  // namespace

bool LocationMqttClient::RecordBleBeacon(const char (&mac)[18], int rssi) {
    // The scanner formats addresses in uppercase and 127 means RSSI is unavailable.
    if (std::strstr(mac, "D9:62:62") == nullptr || rssi == 127) return false;

    std::lock_guard lock(ble_beacon_mutex);
    auto slot = std::find_if(ble_beacons.begin(), ble_beacons.end(), [&](const BleBeaconSample& sample) {
        return std::strcmp(sample.mac, mac) == 0;
    });
    if (slot == ble_beacons.end()) {
        slot = std::min_element(ble_beacons.begin(), ble_beacons.end(),
            [](const BleBeaconSample& left, const BleBeaconSample& right) {
                const bool left_empty = left.mac[0] == '\0';
                const bool right_empty = right.mac[0] == '\0';
                if (left_empty != right_empty) return left_empty;
                return left.seen_us < right.seen_us;
            });
        std::memcpy(slot->mac, mac, sizeof(slot->mac));
    }
    slot->rssi = rssi;
    slot->seen_us = esp_timer_get_time();
    return true;
}

LocationMqttClient::LocationMqttClient(RndisBoard& board, std::function<bool()> network_ready)
    : board_(board), position_subscriber_(std::make_unique<PositionSubscriber>(board, std::move(network_ready))) {
    Settings settings(kSettingsNamespace);
    group_id_ = settings.GetString("group_id");
    // Historical user_guid in NVS is deliberately not an active boot identity.
    signal_strength_ = settings.GetString("signal_strength");
    runtime_base_seconds_ = static_cast<uint64_t>(
        std::max<int32_t>(0, settings.GetInt("runtime_s", 0)));

    auto& app = Application::GetInstance();
    user_playback_baseline_seconds_ = app.GetSdAudioPlaybackDurationSeconds();
    user_conversation_baseline_ = app.GetConversationCount();
    content_manager_ = std::make_unique<SdContentManager>(
        [this](const std::string& payload) {
            PublishCommandPayload(payload);
        });
}

LocationMqttClient::~LocationMqttClient() {
    Stop();
    position_subscriber_.reset();
    content_manager_.reset();
}

void LocationMqttClient::ReplayContent(bool previous) {
    if (content_manager_) {
        content_manager_->Replay(previous);
    }
}

void LocationMqttClient::Start() {
    if (running_.exchange(true)) {
        ESP_LOGW(kTag, "customer MQTT task is already running");
        return;
    }

    stop_requested_ = false;
    ESP_LOGI(kTag, "Shared MQTT manager: position polling reuses cabinet_mqtt; saved internal stack=6144 bytes");
    if (xTaskCreate(TaskEntry, "cabinet_mqtt", 12288, this, 3,
                    &task_handle_) != pdPASS) {
        task_handle_ = nullptr;
        running_ = false;
        SetScreenDiagnostic("MQTT TASK NO MEM");
        ESP_LOGE(kTag, "failed to create customer MQTT task");
    }
}

void LocationMqttClient::SetScreenDiagnostic(const char* message, bool fault) {
    if (auto* display = board_.GetDisplay()) {
        if (fault) display->SetDiagnostic(DiagnosticChannel::BusinessMqtt, message);
        else display->SetDiagnosticStatus(DiagnosticChannel::BusinessMqtt, message);
    }
}

void LocationMqttClient::Stop() {
    stop_requested_ = true;
    TaskHandle_t task = task_handle_;
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
    // Network and AT work can outlast one polling cycle. Join before releasing the
    // positioning client that this worker now services.
    while (running_) vTaskDelay(pdMS_TO_TICKS(10));
    task_handle_ = nullptr;
    CloseBlueScanSocket();
    PersistRuntimeCheckpoint();
    StopMqtt();
}

void LocationMqttClient::TaskEntry(void* arg) {
    auto* client = static_cast<LocationMqttClient*>(arg);
    client->Run();
    client->CloseBlueScanSocket();
    // Quiesce business MQTT callbacks before releasing their notification task.
    client->StopMqtt();
    client->task_handle_ = nullptr;
    client->running_ = false;
    vTaskDelete(nullptr);
}

bool LocationMqttClient::IsNetworkAvailable() const {
    return !board_.IsCellularNetwork() || board_.IsNetworkReady();
}

bool LocationMqttClient::WaitOrStop(uint32_t delay_ms, bool interrupt_on_network_loss) {
    const int64_t deadline = esp_timer_get_time() + int64_t(delay_ms) * 1000;
    while (!stop_requested_) {
        // Keep the independent position connection/CSQ alive even during
        // business MQTT retries, without a second 6144-byte worker stack.
        position_subscriber_->Poll();
        PollBlueScan();
        if (interrupt_on_network_loss && !IsNetworkAvailable()) break;
        const int64_t remaining_ms = (deadline - esp_timer_get_time()) / 1000;
        if (remaining_ms <= 0) break;
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(std::min<int64_t>(remaining_ms, 1000)))) break;
    }
    return stop_requested_;
}

void LocationMqttClient::Run() {
    if (!*JIUCHUAN_BUSINESS_MQTT_USERNAME || !*JIUCHUAN_BUSINESS_MQTT_PASSWORD) {
        SetScreenDiagnostic("BIZ MQTT NO CONFIG");
        ESP_LOGW(kTag, "Missing business MQTT credentials for %s",
                 BrokerAddressForLog(kBusinessBrokerUri).c_str());
        while (!WaitOrStop(1000)) {
            // Keep positioning and BlueScan available without business credentials.
        }
        return;
    }

    while (!stop_requested_) {
        position_subscriber_->Poll();
        PollBlueScan();
        if (!IsNetworkAvailable()) {
            SetScreenDiagnostic("BIZ MQTT WAIT NET", false);
            if (WaitOrStop(1000)) break;
            continue;
        }

        const std::string physical_imei = board_.GetImei();
        if (physical_imei.empty()) {
            SetScreenDiagnostic("BIZ MQTT WAIT IMEI", false);
            ESP_LOGW(kTag, "physical IMEI is not ready; business MQTT connection postponed");
            if (WaitOrStop(kRetryDelayMs)) {
                break;
            }
            continue;
        }
        if (!StartMqtt(physical_imei)) {
            if (WaitOrStop(kRetryDelayMs)) {
                break;
            }
            continue;
        }

        uint32_t waited_ms = 0;
        while (!stop_requested_ && !connected_ && IsNetworkAvailable() && waited_ms < kConnectWaitMs) {
            if (WaitOrStop(1000)) {
                break;
            }
            waited_ms += 1000;
        }
        if (stop_requested_) {
            break;
        }
        if (!IsNetworkAvailable()) {
            StopMqtt();
            continue;
        }
        if (!connected_) {
            if (!connect_error_reported_) SetScreenDiagnostic("BIZ MQTT LOGIN TIMEOUT");
            ESP_LOGE(kTag, "Business MQTT did not connect within %u ms", kConnectWaitMs);
            StopMqtt();
            if (WaitOrStop(kRetryDelayMs)) {
                break;
            }
            continue;
        }

        QuerySignalStrength();
        if (PublishOnlineReport()) {
            PersistRuntimeCheckpoint();
            report_requested_ = false;
            Application::GetInstance().NotifyCustomerMqttOnline();
        } else {
            report_requested_ = true;
        }

        uint32_t disconnected_ms = 0;
        uint32_t report_elapsed_seconds = 0;
        while (!stop_requested_) {
            if (WaitOrStop(1000)) {
                break;
            }
            if (!IsNetworkAvailable()) break;
            if (!connected_) {
                disconnected_ms += 1000;
                if (disconnected_ms >= kReconnectGraceMs) {
                    ESP_LOGW(kTag,
                             "MQTT remained offline for %u ms; restarting the client",
                             disconnected_ms);
                    break;
                }
                continue;
            }

            disconnected_ms = 0;
            ++report_elapsed_seconds;
            if (report_requested_.exchange(false) ||
                report_elapsed_seconds >= kOnlineReportIntervalSeconds) {
                QuerySignalStrength();
                PublishOnlineReport();
                PersistRuntimeCheckpoint();
                report_elapsed_seconds = 0;
            }
        }

        StopMqtt();
        if (!stop_requested_ && WaitOrStop(1000)) {
            break;
        }
    }
}

void LocationMqttClient::CloseBlueScanSocket() {
    if (blue_scan_socket_ < 0) return;
    close(blue_scan_socket_);
    blue_scan_socket_ = -1;
}

void LocationMqttClient::PollBlueScan() {
    const auto status = [this](const char* text) {
        if (auto* display = board_.GetDisplay()) {
            display->SetDiagnosticStatus(DiagnosticChannel::BlueScan, text);
        }
    };
    esp_netif_ip_info_t ip{};
    if (!board_.GetRndisIpInfo(ip)) {
        CloseBlueScanSocket();
        blue_scan_local_ip_ = blue_scan_gateway_ = 0;
        blue_scan_next_send_us_ = 0;
        status("BLUE WAIT IP");
        return;
    }
    if (blue_scan_local_ip_ != ip.ip.addr || blue_scan_gateway_ != ip.gw.addr) {
        CloseBlueScanSocket();
        blue_scan_local_ip_ = ip.ip.addr;
        blue_scan_gateway_ = ip.gw.addr;
        blue_scan_next_send_us_ = 0;
    }

    const int64_t now = esp_timer_get_time();
    if (blue_scan_socket_ >= 0) {
        sockaddr_in sender{};
        socklen_t sender_size = sizeof(sender);
        char reply[32];
        const auto size = recvfrom(blue_scan_socket_, reply, sizeof(reply), MSG_DONTWAIT,
                                   reinterpret_cast<sockaddr*>(&sender), &sender_size);
        if (size > 0 && size < static_cast<ssize_t>(sizeof(reply)) &&
            sender.sin_family == AF_INET && sender.sin_addr.s_addr == blue_scan_gateway_) {
            const std::string_view message(reply, size);
            const auto first = message.find_first_not_of(" \t\r\n");
            if (first != std::string_view::npos &&
                message.substr(first, message.find_last_not_of(" \t\r\n") - first + 1) == "OK") {
                char diagnostic[24];
                std::snprintf(diagnostic, sizeof(diagnostic), "BLUE OK %d", blue_scan_sent_count_);
                status(diagnostic);
                ESP_LOGI(kTag, "BlueScan acknowledged, beacons=%d", blue_scan_sent_count_);
                CloseBlueScanSocket();
                return;
            }
        }
        if (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            ESP_LOGW(kTag, "BlueScan UDP receive failed: errno=%d", errno);
            status("BLUE UDP ERROR");
        } else if (now >= blue_scan_ack_deadline_us_) {
            ESP_LOGW(kTag, "BlueScan UDP ACK timeout");
            status("BLUE NO ACK");
        } else {
            return;
        }
        CloseBlueScanSocket();
        return;
    }
    if (now < blue_scan_next_send_us_) return;
    blue_scan_next_send_us_ = now + kBlueScanIntervalUs;

    auto list = BuildBleBeaconList("mac");
    std::unique_ptr<char, decltype(&cJSON_free)> json(list ? cJSON_PrintUnformatted(list.get()) : nullptr, cJSON_free);
    if (!json) {
        status("BLUE JSON ERROR");
        return;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = blue_scan_local_ip_;
    sockaddr_in gateway{};
    gateway.sin_family = AF_INET;
    gateway.sin_port = htons(kBlueScanPort);
    gateway.sin_addr.s_addr = blue_scan_gateway_;
    const size_t length = std::strlen(json.get());
    blue_scan_socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (blue_scan_socket_ < 0 ||
        bind(blue_scan_socket_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0 ||
        sendto(blue_scan_socket_, json.get(), length, MSG_DONTWAIT,
               reinterpret_cast<const sockaddr*>(&gateway), sizeof(gateway)) != static_cast<ssize_t>(length)) {
        ESP_LOGW(kTag, "BlueScan UDP send failed: errno=%d", errno);
        CloseBlueScanSocket();
        status("BLUE UDP ERROR");
        return;
    }
    blue_scan_sent_count_ = cJSON_GetArraySize(list.get());
    blue_scan_ack_deadline_us_ = esp_timer_get_time() + kBlueScanAckTimeoutUs;
    status("BLUE WAIT OK");
    ESP_LOGI(kTag, "BlueScan sent to " IPSTR ":%u, beacons=%d", IP2STR(&ip.gw),
             static_cast<unsigned>(kBlueScanPort), blue_scan_sent_count_);
}

bool LocationMqttClient::StartMqtt(const std::string& physical_imei) {
    StopMqtt();
    connect_error_reported_ = false;
    std::lock_guard<std::mutex> lock(mqtt_mutex_);

    physical_imei_ = physical_imei;
    cab_guid_ = physical_imei_ + "s3";
    const std::string topic_prefix = "topic/" + cab_guid_ + "/";
    command_topic_ = topic_prefix + "cmd";
    event_topic_ = topic_prefix + "event";
    position_topic_ = topic_prefix + "position";
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        group_topic_ = group_id_.empty() ? "" : kGroupTopicPrefix + group_id_;
    }

    esp_mqtt_client_config_t config = {};
    config.broker.address.uri = kBusinessBrokerUri;
    config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    config.credentials.client_id = cab_guid_.c_str();
    config.credentials.username = JIUCHUAN_BUSINESS_MQTT_USERNAME;
    config.credentials.authentication.password = JIUCHUAN_BUSINESS_MQTT_PASSWORD;
    config.session.keepalive = 60;
    config.network.reconnect_timeout_ms = 5000;
    config.network.timeout_ms = 15000;
    config.task.stack_size = 6144;
    config.buffer.size = 4096;

    ESP_LOGI(kTag, "[CONNECT] broker=%s clientId=%s keepalive=%d stack=%d buffer=%d internal_free=%u internal_largest=%u psram_free=%u",
             BrokerAddressForLog(kBusinessBrokerUri).c_str(), cab_guid_.c_str(),
             config.session.keepalive, config.task.stack_size, config.buffer.size,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    mqtt_client_ = esp_mqtt_client_init(&config);
    if (!mqtt_client_) {
        SetScreenDiagnostic("BIZ MQTT INIT ERROR");
        ESP_LOGE(kTag, "[INIT_FAILED] broker=%s internal_free=%u internal_largest=%u",
                 BrokerAddressForLog(kBusinessBrokerUri).c_str(),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        return false;
    }
    esp_err_t result = esp_mqtt_client_register_event(
        mqtt_client_, MQTT_EVENT_ANY, MqttEventEntry, this);
    const char* stage = result == ESP_OK ? "create_task" : "register_event";
    if (result == ESP_OK) {
        result = esp_mqtt_client_start(mqtt_client_);
    }
    if (result != ESP_OK) {
        SetScreenDiagnostic("BIZ MQTT START ERROR");
        ESP_LOGE(kTag, "[START_FAILED] broker=%s stage=%s error=%s stack=%d internal_free=%u internal_largest=%u; broker login has not started",
                 BrokerAddressForLog(kBusinessBrokerUri).c_str(), stage, esp_err_to_name(result), config.task.stack_size,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        esp_mqtt_client_destroy(mqtt_client_);
        mqtt_client_ = nullptr;
        return false;
    }

    ESP_LOGI(kTag, "[TASK_STARTED] broker=%s; waiting for broker CONNACK",
             BrokerAddressForLog(kBusinessBrokerUri).c_str());
    return true;
}

void LocationMqttClient::StopMqtt() {
    std::lock_guard<std::mutex> lock(mqtt_mutex_);
    connected_ = false;
    if (mqtt_client_) {
        esp_mqtt_client_stop(mqtt_client_);
        esp_mqtt_client_destroy(mqtt_client_);
        mqtt_client_ = nullptr;
    }
    incoming_topic_.clear();
    incoming_payload_.clear();
    incoming_retained_ = false;
}

void LocationMqttClient::MqttEventEntry(void* handler_args,
                                        esp_event_base_t,
                                        int32_t event_id,
                                        void* event_data) {
    static_cast<LocationMqttClient*>(handler_args)->HandleMqttEvent(
        event_id, static_cast<esp_mqtt_event_handle_t>(event_data));
}

void LocationMqttClient::SubscribeBusinessTopics(
        esp_mqtt_client_handle_t client) {
    const int command_id = esp_mqtt_client_subscribe(
        client, command_topic_.c_str(), 0);
    const int event_id = esp_mqtt_client_subscribe(
        client, event_topic_.c_str(), 0);
    ESP_LOGI(kTag, "subscribe %s, message_id=%d",
             command_topic_.c_str(), command_id);
    ESP_LOGI(kTag, "subscribe %s, message_id=%d",
             event_topic_.c_str(), event_id);

    const int position_id = esp_mqtt_client_subscribe(
        client, position_topic_.c_str(), 0);
    ESP_LOGI(kTag, "subscribe business position %s, message_id=%d",
             position_topic_.c_str(), position_id);

    std::string group_topic;
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        group_topic = group_topic_;
    }
    if (!group_topic.empty()) {
        const int group_id = esp_mqtt_client_subscribe(
            client, group_topic.c_str(), 0);
        ESP_LOGI(kTag, "subscribe cached group %s, message_id=%d",
                 group_topic.c_str(), group_id);
    }
}

void LocationMqttClient::HandleMqttEvent(int32_t event_id,
                                         esp_mqtt_event_handle_t event) {
    switch (event_id) {
        case MQTT_EVENT_BEFORE_CONNECT:
            ESP_LOGI(kTag, "[LOGIN] broker=%s clientId=%s", BrokerAddressForLog(kBusinessBrokerUri).c_str(), cab_guid_.c_str());
            break;
        case MQTT_EVENT_CONNECTED:
            connected_ = true;
            SetScreenDiagnostic("BIZ MQTT ONLINE", false);
            ESP_LOGI(kTag, "[CONNECTED] broker=%s clientId=%s CONNACK=0", BrokerAddressForLog(kBusinessBrokerUri).c_str(), cab_guid_.c_str());
            SubscribeBusinessTopics(event->client);
            report_requested_ = true;
            if (task_handle_ != nullptr) {
                xTaskNotifyGive(task_handle_);
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            if (connected_.exchange(false)) SetScreenDiagnostic("BIZ MQTT OFFLINE");
            incoming_topic_.clear();
            incoming_payload_.clear();
            incoming_retained_ = false;
            ESP_LOGW(kTag, "[DISCONNECTED] broker=%s; automatic reconnect is active", BrokerAddressForLog(kBusinessBrokerUri).c_str());
            break;
        case MQTT_EVENT_SUBSCRIBED:
            if (event->error_handle && event->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
                SetScreenDiagnostic("BIZ MQTT SUB DENIED");
                ESP_LOGW(kTag, "[SUB_DENIED] broker=%s message_id=%d",
                         BrokerAddressForLog(kBusinessBrokerUri).c_str(), event->msg_id);
            } else {
                ESP_LOGI(kTag, "[SUBSCRIBED] broker=%s message_id=%d",
                         BrokerAddressForLog(kBusinessBrokerUri).c_str(), event->msg_id);
            }
            break;
        case MQTT_EVENT_ERROR:
            connected_ = false;
            connect_error_reported_ = true;
            SetScreenDiagnostic("BIZ MQTT LINK ERROR");
            if (event->error_handle) {
                const auto* error = event->error_handle;
                if (error->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                    char diagnostic[40];
                    std::snprintf(diagnostic, sizeof(diagnostic), "BIZ MQTT CONNACK %d",
                                  int(error->connect_return_code));
                    SetScreenDiagnostic(diagnostic);
                } else if (error->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
                    SetScreenDiagnostic("BIZ MQTT SUB ERROR");
                }
                ESP_LOGE(kTag, "[ERROR] broker=%s type=%d connack=%d tls_esp=0x%x tls_stack=%d socket_errno=%d",
                         BrokerAddressForLog(kBusinessBrokerUri).c_str(), int(error->error_type), int(error->connect_return_code),
                         int(error->esp_tls_last_esp_err), error->esp_tls_stack_err, error->esp_transport_sock_errno);
            } else ESP_LOGE(kTag, "[ERROR] broker=%s (no error detail)", BrokerAddressForLog(kBusinessBrokerUri).c_str());
            break;
        case MQTT_EVENT_DATA:
            if (event->current_data_offset == 0) {
                incoming_topic_.assign(event->topic, event->topic_len);
                incoming_payload_.clear();
                incoming_payload_.reserve(event->total_data_len);
                incoming_retained_ = event->retain;
            }
            incoming_payload_.append(event->data, event->data_len);
            if (incoming_payload_.size() >=
                static_cast<size_t>(event->total_data_len)) {
                HandleMqttPayload(incoming_topic_, incoming_payload_, incoming_retained_);
                incoming_topic_.clear();
                incoming_payload_.clear();
                incoming_retained_ = false;
            }
            break;
        default:
            break;
    }
}

void LocationMqttClient::UpdateGroupSubscription(
    const std::string& old_topic, const std::string& new_topic) {
    if (!connected_ || !mqtt_client_ || old_topic == new_topic) {
        return;
    }
    if (!old_topic.empty()) {
        const int message_id = esp_mqtt_client_unsubscribe(
            mqtt_client_, old_topic.c_str());
        ESP_LOGI(kTag, "unsubscribe old group %s, message_id=%d",
                 old_topic.c_str(), message_id);
    }
    if (!new_topic.empty()) {
        const int message_id = esp_mqtt_client_subscribe(
            mqtt_client_, new_topic.c_str(), 0);
        ESP_LOGI(kTag, "subscribe new group %s, message_id=%d",
                 new_topic.c_str(), message_id);
    }
}

void LocationMqttClient::HandleMqttPayload(const std::string& topic,
                                           const std::string& payload,
                                           bool retained) {
    ESP_LOGI(kTag, "RX topic=%s payload_length=%u", topic.c_str(),
             static_cast<unsigned>(payload.size()));

    std::string current_group_topic;
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        current_group_topic = group_topic_;
    }
    if (!current_group_topic.empty() && topic == current_group_topic) {
        ESP_LOGI(kTag, "group broadcast received; print-only mode is active");
        return;
    }

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        cJSON_ParseWithLength(payload.data(), payload.size()), cJSON_Delete);
    if (!root) {
        ESP_LOGW(kTag, "ignored non-JSON MQTT payload on %s", topic.c_str());
        return;
    }
    if (topic == command_topic_) {
        const std::string safe_payload = CommandPayloadForLog(root.get());
        ESP_LOGI(kTag, "RX command topic=%s payload=%s",
                 topic.c_str(), safe_payload.c_str());
    }
    cJSON* message = root.get();
    cJSON* nested = cJSON_GetObjectItemCaseSensitive(root.get(), "payload");
    if (cJSON_IsObject(nested)) {
        message = nested;
    }

    // Direct ML307C telemetry is handled by PositionSubscriber on its own broker.
    if (topic == position_topic_) {
        return;
    }

    if (topic == command_topic_ &&
        (cJSON_HasObjectItem(message, "deviceExecute") ||
         cJSON_HasObjectItem(message, "downSDfile") ||
         cJSON_HasObjectItem(message, "bindingAuto") ||
         BindingJsonString(message, "cmd") == "bindingAuto")) {
        ESP_LOGD(kTag, "ignored response received on shared command topic");
        return;
    }

    if (topic == command_topic_) {
        // Group assignment is independent of identity and never restored from
        // a retained command, including one carrying a legacy binding request.
        std::string new_group_id;
        const bool has_group_id = !retained && TryFirstJsonScalar(
            message, {"groupID", "groupId", "group_id"}, new_group_id);

        std::string old_group_topic;
        std::string new_group_topic;
        bool settings_changed = false;
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            old_group_topic = group_topic_;
            if (has_group_id && new_group_id != group_id_) {
                group_id_ = new_group_id;
                group_topic_ = group_id_.empty()
                    ? ""
                    : kGroupTopicPrefix + group_id_;
                settings_changed = true;
            }
            new_group_topic = group_topic_;
        }

        if (settings_changed) {
            Settings settings(kSettingsNamespace, true);
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            settings.SetString("group_id", group_id_);
        }
        UpdateGroupSubscription(old_group_topic, new_group_topic);
        if (has_group_id && !new_group_id.empty()) {
            Application::GetInstance().HandleContentGroupId(new_group_id);
        }
        report_requested_ = true;

        const std::string binding_command = BindingJsonString(message, "cmd");
        const bool binding_requested = binding_command == "bindUser" ||
            binding_command == "unbindUser" ||
            cJSON_HasObjectItem(message, "userGUID") ||
            cJSON_HasObjectItem(message, "userGuid") ||
            cJSON_HasObjectItem(message, "user_guid") ||
            cJSON_HasObjectItem(message, "bindingToken") ||
            cJSON_HasObjectItem(message, "bindingVersion");
        if (binding_requested) {
            if (retained) {
                ESP_LOGW(kTag, "rejected retained user binding request; identity unchanged");
                PublishExecution(BindingJsonString(message, "cmdID"), false,
                                 "retained user binding requests are not accepted");
                return;
            }
            if (IsLegacyUserBindingRequest(message)) {
                ESP_LOGI(kTag,
                         "legacy user binding requested; awaiting server-issued bindUser; identity unchanged");
                std::unique_ptr<cJSON, decltype(&cJSON_Delete)> response(
                    cJSON_CreateObject(), cJSON_Delete);
                if (response) {
                    AddString(response.get(), "deviceExecute", "pending");
                    AddString(response.get(), "cmdID", BindingJsonString(message, "cmdID"));
                    AddString(response.get(), "msg",
                              "awaiting server-issued bindUser; user identity unchanged");
                    char* response_text = cJSON_PrintUnformatted(response.get());
                    if (response_text) {
                        PublishCommandPayload(response_text);
                        cJSON_free(response_text);
                    }
                }
                return;
            }
            const auto command = ParseUserBindingCommand(message);
            Application::GetInstance().ApplyUserBinding(command,
                [this, command_id = command.command_id](BindingCommandResult result,
                                                       const std::string& error) {
                    if (result == BindingCommandResult::Accepted) {
                        auto& app = Application::GetInstance();
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        user_playback_baseline_seconds_ = app.GetSdAudioPlaybackDurationSeconds();
                        user_conversation_baseline_ = app.GetConversationCount();
                    }
                    PublishExecution(command_id, result != BindingCommandResult::Rejected, error);
                    RequestOnlineReport();
                });
            return;
        }

        const std::string device = JsonScalar(message, "device");
        if (device == "Control") {
            const std::string command_id = JsonScalar(message, "cmdID");
            const std::string power = JsonScalar(message, "power");
            const std::string volume = JsonScalar(message, "volume");
            const std::string role_id = JsonScalar(message, "roleID");
            const std::string network_type = JsonScalar(message, "net");
            std::string wifi_credentials = JsonScalar(message, "WIFI PW");
            if (wifi_credentials.empty()) {
                wifi_credentials = JsonScalar(message, "WIFIPW");
            }
            if (wifi_credentials.empty()) {
                wifi_credentials = JsonScalar(message, "wifiPW");
            }
            if (wifi_credentials.empty()) {
                wifi_credentials = JsonScalar(message, "wifi_pw");
            }
            bool success = true;
            bool network_reboot_required = false;
            std::string error;
            bool rgb1_requested = false;
            bool rgb2_requested = false;
            std::vector<IndexedRgbColor> rgb1_pixels;
            std::vector<IndexedRgbColor> rgb2_pixels;
            if (!ParseRgbField(message, "rgb1", BUILTIN_LED_NUM,
                               BUILTIN_LED_NUM, rgb1_requested,
                               rgb1_pixels, error)) {
                success = false;
            }
            if (success &&
                !ParseRgbField(message, "rgb2", EXTERNAL_LED_NUM,
                               EXTERNAL_LED_MAX_ACTIVE, rgb2_requested,
                               rgb2_pixels, error)) {
                success = false;
            }
            const bool network_requested = !IsEmptyCommandValue(network_type);
            const bool power_requested = !IsEmptyCommandValue(power);
            const bool volume_requested = !IsEmptyCommandValue(volume);
            const bool role_requested = !IsEmptyCommandValue(role_id);
            const bool rgb_requested = rgb1_requested || rgb2_requested;
            const bool any_control_requested = network_requested || power_requested ||
                                               volume_requested || role_requested ||
                                               rgb_requested;
            int requested_internal_volume = -1;

            if (any_control_requested && command_id.empty()) {
                success = false;
                error = "cmdID is required for control commands";
            }

            if (network_requested && success) {
                if (!board_.ConfigureNetwork(
                               network_type, wifi_credentials, false,
                               network_reboot_required, error)) {
                    success = false;
                }
            }

            if (power_requested && success) {
                std::string normalized_power = Trim(power);
                std::transform(normalized_power.begin(), normalized_power.end(),
                               normalized_power.begin(), [](unsigned char ch) {
                                   return static_cast<char>(std::tolower(ch));
                               });
                if (normalized_power != "off") {
                    success = false;
                    error = "power must be off";
                } else if (network_requested || volume_requested || role_requested ||
                           rgb_requested) {
                    success = false;
                    error = "power=off must be sent without other control actions";
                } else if (shutdown_scheduled_) {
                    success = false;
                    error = "shutdown is already scheduled";
                }
            }

            if (volume_requested && success) {
                char* end = nullptr;
                errno = 0;
                const long percent = std::strtol(volume.c_str(), &end, 10);
                if (errno != 0 || end == volume.c_str() || *end != '\0' ||
                    percent < 0 || percent > 100) {
                    success = false;
                    error = "volume must be an integer from 0 to 100";
                } else {
                    requested_internal_volume =
                        static_cast<int>((percent * 80 + 50) / 100);
                }
            }

            auto finish_command =
                [this, command_id, network_type, wifi_credentials,
                 network_requested, network_reboot_required, power_requested,
                 requested_internal_volume, rgb1_requested, rgb1_pixels,
                 rgb2_requested, rgb2_pixels](
                    bool command_success, const std::string& command_error) {
                    bool apply_success = command_success;
                    std::string apply_error = command_error;
                    if (apply_success && (rgb1_requested || rgb2_requested) &&
                        !board_.ApplyRgbControl(
                            rgb1_requested, rgb1_pixels,
                            rgb2_requested, rgb2_pixels, apply_error)) {
                        apply_success = false;
                    }
                    if (apply_success && requested_internal_volume >= 0) {
                        board_.GetAudioCodec()->SetOutputVolume(
                            requested_internal_volume);
                        report_requested_ = true;
                    }
                    const bool response_published =
                        PublishExecution(command_id, apply_success, apply_error);
                    if (apply_success && power_requested && response_published) {
                        if (!SchedulePowerOff()) {
                            ESP_LOGE(kTag, "failed to schedule remote shutdown");
                            PublishExecution(command_id, false,
                                             "failed to schedule remote shutdown");
                        }
                        return;
                    }
                    if (apply_success && network_requested &&
                        network_reboot_required && response_published) {
                        bool persisted_reboot = false;
                        std::string persist_error;
                        if (board_.ConfigureNetwork(
                                network_type, wifi_credentials, true,
                                persisted_reboot, persist_error) && persisted_reboot) {
                            ScheduleNetworkReboot();
                        } else if (!persist_error.empty()) {
                            ESP_LOGE(kTag, "failed to persist network switch: %s",
                                     persist_error.c_str());
                        }
                    }
                };

            if (role_requested) {
                if (!success) {
                    finish_command(false, error);
                } else {
                    Application::GetInstance().SelectRoleVoiceBinding(
                        role_id,
                        [this, finish_command](bool selected) {
                            finish_command(selected, selected ? "" :
                                           "role binding selection failed");
                            if (selected) {
                                report_requested_ = true;
                            }
                        });
                }
            } else {
                finish_command(success, error);
            }
            return;
        }

        const std::string sync_mode = JsonScalar(message, "updateSDfile");
        if (!sync_mode.empty()) {
            SdContentManager::SyncRequest request;
            request.mode = sync_mode;
            request.command_id = JsonScalar(message, "cmdID");
            request.group_id = JsonScalar(message, "groupID");
            request.filelist_url = JsonScalar(message, "filelist");
            if (request.group_id.empty()) {
                std::lock_guard<std::mutex> state_lock(state_mutex_);
                request.group_id = group_id_;
            }
            if (!content_manager_ || !content_manager_->StartSync(request)) {
                cJSON* response = cJSON_CreateObject();
                if (!response) {
                    return;
                }
                AddString(response, "downSDfile", "error");
                AddString(response, "cmdID", request.command_id);
                AddString(response, "progress", "0%");
                AddString(response, "groupID", request.group_id);
                AddString(response, "filelist", request.filelist_url);
                AddString(response, "msg", "another SD sync is already running");
                char* response_text = cJSON_PrintUnformatted(response);
                cJSON_Delete(response);
                if (response_text) {
                    PublishCommandPayload(response_text);
                    cJSON_free(response_text);
                }
            }
            return;
        }
    }

    if (topic == event_topic_ || topic == command_topic_) {
        cJSON* player = cJSON_GetObjectItemCaseSensitive(message, "player");
        const char* player_key = "player";
        if (!player) {
            player = cJSON_GetObjectItemCaseSensitive(message, " player");
            player_key = " player";
        }
        if (player) {
            SdContentManager::PlaybackRequest request;
            request.player = cJSON_IsNumber(player)
                ? player->valueint
                : std::atoi(cJSON_IsString(player) && player->valuestring
                                ? player->valuestring : "0");
            request.command_id = JsonScalar(message, "cmdID");
            request.group_id = JsonScalar(message, "groupID");
            TryFirstJsonScalar(message,
                               {"roleID", "RoleID", "roleId", "role_id"},
                               request.role_id);
            request.role_name = JsonScalar(message, "roleName");
            if (request.role_name.empty()) {
                request.role_name = JsonScalar(message, "rolename");
            }
            request.scene_content_id = JsonScalar(message, "scene_contentID");
            request.pre_sound = JsonScalar(message, "pre_sound");
            request.post_sound = JsonScalar(message, "post_sound");
            request.text = JsonScalar(message, "txt");
            request.picture = JsonScalar(message, "picture");
            if (request.group_id.empty()) {
                std::lock_guard<std::mutex> state_lock(state_mutex_);
                request.group_id = group_id_;
            }
            ESP_LOGI(kTag,
                     "[PLAY_CMD][PARSED] player_key='%s' player=%d cmdID=%s groupID=%s roleID=%s scene=%s pre=%s post=%s picture=%s",
                     player_key, request.player,
                     request.command_id.empty() ? "<none>" : request.command_id.c_str(),
                     request.group_id.empty() ? "<empty>" : request.group_id.c_str(),
                     request.role_id.empty() ? "<empty>" : request.role_id.c_str(),
                     request.scene_content_id.empty() ? "<empty>" : request.scene_content_id.c_str(),
                     request.pre_sound.empty() ? "<none>" : request.pre_sound.c_str(),
                     request.post_sound.empty() ? "<none>" : request.post_sound.c_str(),
                     request.picture.empty() ? "<none>" : request.picture.c_str());
            if (!content_manager_ || !content_manager_->QueuePlayback(request)) {
                ESP_LOGE(kTag,
                         "[PLAY_CMD][REJECT] content playback manager unavailable or queue full");
                PublishExecution(request.command_id, false,
                                 "content playback queue is full");
            }
            return;
        }

        cJSON* file = cJSON_GetObjectItemCaseSensitive(message, "file");
        const std::string filename = cJSON_IsString(file)
            ? NormalizeAudioFile(file->valuestring)
            : std::string();
        if (!filename.empty()) {
            ESP_LOGI(kTag, "received SD audio command: %s", filename.c_str());
            Application::GetInstance().PlaySdAudioFile(filename);
        }
    }
}

void LocationMqttClient::QuerySignalStrength() {
    if (!board_.IsCellularNetwork()) {
        ESP_LOGD(kTag, "WiFi mode active; retaining last cellular CSQ value");
        return;
    }
    std::string response;
    if (!board_.ExecuteUsbAtCommand("AT+CSQ", response, 3000)) {
        ESP_LOGW(kTag, "AT+CSQ failed; retaining previous signal value");
        return;
    }
    const std::string value = ParseCsq(response);
    if (value.empty()) {
        ESP_LOGW(kTag, "unable to parse AT+CSQ response: %s", response.c_str());
        return;
    }
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        signal_strength_ = value;
    }
    Settings settings(kSettingsNamespace, true);
    settings.SetString("signal_strength", value);
    ESP_LOGI(kTag, "ML307 signal strength: %s", value.c_str());
}

bool LocationMqttClient::PublishCommandPayload(const std::string& payload) {
    std::lock_guard<std::mutex> lock(mqtt_mutex_);
    if (!connected_ || !mqtt_client_ || command_topic_.empty() || payload.empty()) {
        ESP_LOGW(kTag, "cannot publish command response while MQTT is offline");
        return false;
    }
    const int message_id = esp_mqtt_client_publish(
        mqtt_client_, command_topic_.c_str(), payload.c_str(),
        static_cast<int>(payload.size()), 0, 0);
    ESP_LOGI(kTag, "TX topic=%s message_id=%d payload_length=%u",
             command_topic_.c_str(), message_id, static_cast<unsigned>(payload.size()));
    return message_id >= 0;
}

bool LocationMqttClient::PublishExecution(
    const std::string& command_id, bool success,
    const std::string& message) {
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return false;
    }
    AddString(root, "deviceExecute", success ? "OK" : "error");
    AddString(root, "cmdID", command_id);
    if (!message.empty()) {
        AddString(root, "msg", message);
    }
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return false;
    }
    const bool published = PublishCommandPayload(json);
    cJSON_free(json);
    return published;
}

void LocationMqttClient::ScheduleNetworkReboot() {
    ESP_LOGI(kTag, "network configuration saved; reboot scheduled after MQTT response");
    if (xTaskCreate([](void*) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            for (int attempt = 0; attempt < 30 &&
                 Application::GetInstance().GetDeviceState() != kDeviceStateIdle;
                 ++attempt) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            Application::GetInstance().Reboot();
            vTaskDelete(nullptr);
        }, "network_reboot", 4096, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "failed to create delayed network reboot task");
    }
}

bool LocationMqttClient::SchedulePowerOff() {
    bool expected = false;
    if (!shutdown_scheduled_.compare_exchange_strong(expected, true)) {
        return false;
    }

    ESP_LOGW(kTag,
             "remote power-off acknowledged; shutdown starts after MQTT flush delay");
    if (xTaskCreate([](void* arg) {
            auto* client = static_cast<LocationMqttClient*>(arg);
            vTaskDelay(pdMS_TO_TICKS(2000));

            client->Stop();
            Application::GetInstance().PrepareForShutdown();

            std::string error;
            if (!client->board_.RequestShutdown(error)) {
                ESP_LOGE(kTag, "remote shutdown failed: %s",
                         error.empty() ? "unknown error" : error.c_str());
                client->shutdown_scheduled_ = false;
            }
            vTaskDelete(nullptr);
        }, "remote_poweroff", 4096, this, 3, nullptr) != pdPASS) {
        shutdown_scheduled_ = false;
        return false;
    }
    return true;
}

bool LocationMqttClient::PublishOnlineReport() {
    std::lock_guard<std::mutex> mqtt_lock(mqtt_mutex_);
    if (!mqtt_client_ || !connected_ || physical_imei_.empty() ||
        cab_guid_.empty()) {
        return false;
    }
    auto* display = board_.GetDisplay();

    std::string group_id;
    const auto binding = board_.GetUserBinding();
    std::string signal_strength;
    uint64_t playback_baseline_seconds = 0;
    uint32_t conversation_baseline = 0;
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        group_id = group_id_;
        signal_strength = signal_strength_;
        playback_baseline_seconds = user_playback_baseline_seconds_;
        conversation_baseline = user_conversation_baseline_;
    }

    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    board_.GetBatteryLevel(battery_level, charging, discharging);
    battery_level = std::clamp(battery_level, 0, 100);
    int volume_percent = 0;
    if (auto* codec = board_.GetAudioCodec()) {
        volume_percent = std::clamp(codec->output_volume(), 0, 80) * 100 / 80;
    }
    int screen_brightness = 0;
    if (auto* backlight = board_.GetBacklight()) {
        screen_brightness = std::clamp<int>(backlight->brightness(), 0, 100);
    }

    uint64_t capacity_kb = 0;
    uint64_t remaining_kb = 0;
    GetSdCardSpaceKb(capacity_kb, remaining_kb);

    const int64_t now_us = esp_timer_get_time();
    const uint64_t session_seconds = static_cast<uint64_t>(
        std::max<int64_t>(0, now_us) / 1000000);
    const uint64_t total_runtime_seconds =
        runtime_base_seconds_ + session_seconds;

    auto& app = Application::GetInstance();
    const std::string role = app.GetCurrentRoleId();
    const uint64_t all_playback_seconds =
        app.GetSdAudioPlaybackDurationSeconds();
    const uint32_t all_conversations = app.GetConversationCount();
    const uint64_t user_playback_seconds =
        all_playback_seconds >= playback_baseline_seconds
            ? all_playback_seconds - playback_baseline_seconds
            : 0;
    const uint32_t user_conversations =
        all_conversations >= conversation_baseline
            ? all_conversations - conversation_baseline
            : 0;

    const esp_app_desc_t* app_description = esp_app_get_description();
    const std::string firmware_version =
        app_description ? app_description->version : "2.1.0";

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        if (display) display->SetDiagnostic(DiagnosticChannel::OnlineReport, "KJ JSON NO MEM");
        ESP_LOGE(kTag, "failed to allocate KJ JSON");
        return false;
    }
    AddString(root, "cmd", "KJ");
    AddString(root, "cabGuid", cab_guid_);
    AddString(root, "imei", physical_imei_);
    AddString(root, "macAddress", SystemInfo::GetMacAddress());
    AddString(root, "groupID", group_id);
    AddString(root, "userGUID", binding.user_guid);
    AddString(root, "bootID", binding.boot_id);
    AddString(root, "bindingState", BindingStateName(binding.state));
    cJSON_AddNumberToObject(root, "bindingVersion", static_cast<double>(binding.version));
    AddString(root, "usageSessionID", binding.usage_session_id);
    AddString(root, "fwVersion", firmware_version);
    AddString(root, "Battery Level", std::to_string(battery_level));
    AddString(root, "Volume", std::to_string(volume_percent));
    AddString(root, "Screen Brightness", std::to_string(screen_brightness));
    AddString(root, "Signal Strength", signal_strength);
    AddString(root, "TF Card Capacity", std::to_string(capacity_kb));
    AddString(root, "TF Card Remaining Space", std::to_string(remaining_kb));
    AddString(root, "Total Runtime", std::to_string(total_runtime_seconds / 60));
    AddString(root, "Single Session Runtime", std::to_string(session_seconds / 60));
    AddString(root, "Playback Duration", std::to_string(user_playback_seconds));
    AddString(root, "Conversation Count", std::to_string(user_conversations));
    AddString(root, "Role", role);
    const int beacon_count = AddBleBeaconReport(root);

    char* payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        if (display) display->SetDiagnostic(DiagnosticChannel::OnlineReport, "KJ JSON NO MEM");
        ESP_LOGE(kTag, "failed to serialize KJ report");
        return false;
    }

    const std::string online_topic = "topic/" + cab_guid_ + "/online";
    const int message_id = esp_mqtt_client_publish(
        mqtt_client_, online_topic.c_str(), payload, 0, 0, 0);
    ESP_LOGI(kTag, "TX topic=%s message_id=%d payload=%s",
             online_topic.c_str(), message_id, payload);
    cJSON_free(payload);
    if (message_id >= 0) {
        ++online_report_count_;
        if (display && beacon_count >= 0) {
            char diagnostic[40];
            std::snprintf(diagnostic, sizeof(diagnostic), "KJ TX #%lu BLE=%d",
                          static_cast<unsigned long>(online_report_count_), beacon_count);
            display->SetDiagnosticStatus(DiagnosticChannel::OnlineReport, diagnostic);
        } else if (display) {
            display->SetDiagnostic(DiagnosticChannel::OnlineReport, "KJ BLE NO MEM");
        }
    } else if (display) {
        display->SetDiagnostic(DiagnosticChannel::OnlineReport, "KJ SEND ERROR");
    }
    return message_id >= 0;
}

void LocationMqttClient::PersistRuntimeCheckpoint() {
    const int64_t now_us = esp_timer_get_time();
    const uint64_t session_seconds = static_cast<uint64_t>(
        std::max<int64_t>(0, now_us) / 1000000);
    const uint64_t total_runtime_seconds =
        runtime_base_seconds_ + session_seconds;
    const int32_t stored = total_runtime_seconds > INT32_MAX
        ? INT32_MAX
        : static_cast<int32_t>(total_runtime_seconds);
    Settings settings(kSettingsNamespace, true);
    settings.SetInt("runtime_s", stored);
}
