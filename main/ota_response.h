#pragma once

#include <cJSON.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ota_response {

inline bool HasText(const cJSON* object, const char* key) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring && value->valuestring[0];
}

// Run before changing NVS, activation flags or the system clock.
inline bool IsConfiguration(const cJSON* root) {
    if (!cJSON_IsObject(root)) return false;
    const auto* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (code && (!cJSON_IsNumber(code) || code->valuedouble != 0)) return false;
    const auto* error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (error && !cJSON_IsNull(error)) return false;
    const auto* success = cJSON_GetObjectItemCaseSensitive(root, "success");
    if (success && !cJSON_IsTrue(success)) return false;

    const auto* activation = cJSON_GetObjectItemCaseSensitive(root, "activation");
    const auto* mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
    const auto* websocket = cJSON_GetObjectItemCaseSensitive(root, "websocket");
    return (cJSON_IsObject(activation) &&
            (HasText(activation, "code") || HasText(activation, "challenge"))) ||
           (cJSON_IsObject(mqtt) && HasText(mqtt, "endpoint")) ||
           (cJSON_IsObject(websocket) && HasText(websocket, "url"));
}

inline bool IsAlphaNumeric(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

inline std::vector<int> ParseVersion(const std::string& value) {
    std::vector<int> parts;
    size_t pos = 0;
    while (pos < value.size()) {
        int component = 0;
        size_t digits = 0;
        while (pos < value.size() && value[pos] >= '0' && value[pos] <= '9') {
            if (++digits > 9) return {};
            component = component * 10 + value[pos++] - '0';
        }
        if (!digits || parts.size() == 4) return {};
        parts.push_back(component);
        if (pos == value.size()) return parts;
        if (value[pos] == '.') {
            if (++pos == value.size()) return {};
            continue;
        }
        if (value[pos] != '-' && value[pos] != '+') return {};
        bool metadata = value[pos++] == '+';
        bool has_token = false;
        for (; pos < value.size(); ++pos) {
            const char ch = value[pos];
            if (IsAlphaNumeric(ch)) {
                has_token = true;
            } else if (has_token && (ch == '.' || ch == '-' || (ch == '+' && !metadata))) {
                if (ch == '+') metadata = true;
                has_token = false;
            } else {
                return {};
            }
        }
        return has_token ? parts : std::vector<int>{};
    }
    return {};
}

inline bool IsNewer(const std::string& current, const std::string& candidate) {
    const auto a = ParseVersion(current);
    const auto b = ParseVersion(candidate);
    if (a.empty() || b.empty()) return false;
    for (size_t i = 0; i < a.size() || i < b.size(); ++i) {
        const int av = i < a.size() ? a[i] : 0;
        const int bv = i < b.size() ? b[i] : 0;
        if (av != bv) return bv > av;
    }
    return false;
}
}  // namespace ota_response
