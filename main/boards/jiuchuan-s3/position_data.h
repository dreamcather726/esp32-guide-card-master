#pragma once
#include "../../display/navigation_status.h"
#include <cJSON.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>

namespace position {
inline bool Number(const cJSON* item, double& value) {
    if (cJSON_IsNumber(item)) value = item->valuedouble;
    else if (cJSON_IsString(item) && item->valuestring && *item->valuestring) {
        char* end = nullptr;
        errno = 0;
        value = std::strtod(item->valuestring, &end);
        if (errno || end == item->valuestring || *end != '\0') return false;
    } else return false;
    return std::isfinite(value);
}

inline bool Coordinate(const cJSON* item, double limit, double& value) {
    if (!Number(item, value)) return false;
    // ML307C publishes NMEA ddmm.mmmm / dddmm.mmmm. Decimal-degree messages
    // from older integrations are also accepted when already within range.
    const double magnitude = std::fabs(value);
    if (magnitude > limit) {
        const double degrees = std::floor(magnitude / 100);
        const double minutes = magnitude - degrees * 100;
        if (minutes >= 60) return false;
        value = std::copysign(degrees + minutes / 60, value);
    }
    return std::fabs(value) <= limit;
}

inline bool Parse(const std::string& text, NavigationStatus& result) {
    if (text.empty() || text.size() > 4096) return false;
    const char* end = nullptr;
    cJSON* root = cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, &end, true);
    if (!root) return false;
    const cJSON* body = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (!cJSON_IsObject(body)) body = root;
    if (!cJSON_IsObject(body)) { cJSON_Delete(root); return false; }
    const auto field = [body](const char* key) { return cJSON_GetObjectItemCaseSensitive(body, key); };
    const cJSON* longitude = field("longitude");
    if (!longitude) longitude = field("longtitude");
    if (!field("nsat") && !field("latitude") && !longitude && !field("fix")) {
        cJSON_Delete(root); return false;
    }
    NavigationStatus status;
    double satellites = 0, fix = 0;
    if (Number(field("nsat"), satellites) && satellites >= 0 && satellites <= 99 &&
        satellites == std::floor(satellites)) status.satellites = static_cast<int>(satellites);
    status.has_coordinates = Coordinate(field("latitude"), 90, status.latitude) &&
        Coordinate(longitude, 180, status.longitude);
    const bool has_fix = Number(field("fix"), fix) && fix == std::floor(fix) && fix >= 1 && fix <= 5;
    status.has_fix = has_fix && status.has_coordinates &&
        (status.latitude != 0 || status.longitude != 0);
    const cJSON* scene = field("Scene");
    if (!scene) scene = field("scene");
    if (cJSON_IsString(scene) && scene->valuestring) {
        const std::string value(scene->valuestring);
        if (value.size() <= 32 && std::none_of(value.begin(), value.end(), [](unsigned char ch) {
                return ch < 0x20 || ch == 0x7f;
            })) status.scene = value;
    } else {
        double id = 0;
        if (Number(scene, id) && id >= 0 && id <= 9007199254740991.0 && id == std::floor(id)) {
            char number[32]; std::snprintf(number, sizeof(number), "%.0f", id);
            status.scene = number;
        }
    }
    result = std::move(status);
    cJSON_Delete(root);
    return true;
}

inline int SignalBars(int csq) {
    if (csq < 1 || csq > 31) return 0;
    if (csq > 27) return 4;
    if (csq >= 16) return 3;  // 27 belongs to this interval.
    if (csq >= 8) return 2;
    return 1;
}

inline int ParseCsq(const std::string& response) {
    const auto begin = response.find("+CSQ:");
    if (begin == std::string::npos) return -1;
    const char* value = response.c_str() + begin + 5;
    char* end = nullptr;
    const long csq = std::strtol(value, &end, 10);
    if (end == value || (*end != ',' && *end != ' ' && *end != '\r' && *end != '\n')) return -1;
    return csq >= 0 && csq <= 31 ? static_cast<int>(csq) : -1;
}

// MQTT payloads can span several DATA events. Reject gaps, foreign topics,
// retained messages and oversized messages without buffering unbounded data.
class Message {
public:
    void Reset() { bytes_.clear(); total_ = 0; active_ = false; }
    bool Feed(const std::string& expected_topic, const char* topic, int topic_length,
              const char* data, int length, int offset, int total, bool retained,
              NavigationStatus& status) {
        if (offset == 0) {
            Reset();
            if (!topic || topic_length <= 0 || std::string(topic, topic_length) != expected_topic ||
                retained || total <= 0 || total > 4096) return false;
            active_ = true; total_ = total;
        }
        if (!active_ || !data || length < 0 || offset < 0 || total != total_ ||
            offset != static_cast<int>(bytes_.size()) || length > total_ - offset) {
            Reset(); return false;
        }
        bytes_.append(data, static_cast<size_t>(length));
        if (static_cast<int>(bytes_.size()) != total_) return false;
        const bool parsed = Parse(bytes_, status);
        Reset();
        return parsed;
    }
private:
    std::string bytes_;
    int total_ = 0;
    bool active_ = false;
};
}  // namespace position
