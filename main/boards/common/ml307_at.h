#ifndef ML307_AT_H
#define ML307_AT_H

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ml307_at {

struct Interface {
    uint8_t number = 0;
    uint8_t klass = 0;
    uint8_t subclass = 0;
    uint8_t protocol = 0;
    uint8_t endpoints = 0;
};

inline bool IsControlInterface(const Interface& descriptor,
                               const Interface* interfaces, size_t count) {
    if (descriptor.klass == 0xff) {
        return descriptor.endpoints >= 2;
    }
    // CDC ACM control interfaces can have just one notification endpoint.
    // RNDIS (e0/01/03 or 02/02/ff) and CDC data interfaces are never probed.
    if (descriptor.klass != 0x02 || descriptor.subclass != 0x02 ||
        descriptor.protocol > 1 || descriptor.endpoints > 1) {
        return false;
    }
    for (size_t index = 0; index < count; ++index) {
        if (interfaces[index].number == descriptor.number + 1 &&
            interfaces[index].klass == 0x0a && interfaces[index].endpoints == 2) {
            return true;
        }
    }
    return false;
}

inline std::vector<uint8_t> ControlCandidates(const Interface* interfaces, size_t count) {
    std::vector<uint8_t> candidates;
    auto add = [&](const Interface& descriptor) {
        if (IsControlInterface(descriptor, interfaces, count) &&
            std::find(candidates.begin(), candidates.end(), descriptor.number) == candidates.end()) {
            candidates.push_back(descriptor.number);
        }
    };
    for (uint8_t preferred : std::array<uint8_t, 3>{2, 8, 4}) {
        for (size_t index = 0; index < count; ++index) {
            if (interfaces[index].number == preferred) {
                add(interfaces[index]);
            }
        }
    }
    for (size_t index = 0; index < count; ++index) {
        add(interfaces[index]);
    }
    return candidates;
}

inline std::string_view Trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    return begin == std::string_view::npos ? std::string_view{} :
        text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

inline int ParseCsq(std::string_view response) {
    while (!response.empty()) {
        const auto end = response.find_first_of("\r\n");
        const auto line = Trim(response.substr(0, end));
        if (line.substr(0, 5) == "+CSQ:") {
            const auto comma = line.find(',');
            if (comma == std::string_view::npos) return -1;
            const auto field = Trim(line.substr(5, comma - 5));
            if (field.empty()) return -1;
            int csq = -1;
            const auto finish = field.data() + field.size();
            const auto parsed = std::from_chars(field.data(), finish, csq);
            return parsed.ec == std::errc{} && parsed.ptr == finish && csq >= 0 && csq <= 31 ? csq : -1;
        }
        if (end == std::string_view::npos) break;
        response.remove_prefix(end + 1);
    }
    return -1;
}

inline bool IsImei(std::string_view text) {
    return text.size() == 15 &&
        std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }) &&
        text != "000000000000000";
}

inline std::string ExtractImei(std::string_view response) {
    while (!response.empty()) {
        const auto end = response.find_first_of("\r\n");
        auto line = Trim(response.substr(0, end));
        if (line.substr(0, 6) == "+CGSN:" || line.substr(0, 5) == "+GSN:") {
            line = Trim(line.substr(line.find(':') + 1));
        }
        if (line.size() >= 2 && line.front() == '"' && line.back() == '"') {
            line = line.substr(1, line.size() - 2);
        }
        if (IsImei(line)) {
            return std::string(line);
        }
        if (end == std::string_view::npos) {
            break;
        }
        response.remove_prefix(end + 1);
    }
    return {};
}

enum class Reply { Pending, Ok, Error };

inline Reply ParseReply(std::string_view response) {
    Reply reply = Reply::Pending;
    while (!response.empty()) {
        const auto end = response.find_first_of("\r\n");
        if (end == std::string_view::npos) {
            break;  // A fragmented terminal line is not complete yet.
        }
        const auto line = Trim(response.substr(0, end));
        if (line == "ERROR" || line.substr(0, 10) == "+CME ERROR" ||
            line.substr(0, 10) == "+CMS ERROR") {
            return Reply::Error;
        }
        if (line == "OK") {
            reply = Reply::Ok;
        }
        response.remove_prefix(end + 1);
    }
    return reply;
}

inline uint32_t RetryDelayMs(unsigned failure_count) {
    return std::min<uint32_t>(30000, 2000U << std::min<unsigned>(failure_count, 4));
}

}  // namespace ml307_at
#endif
