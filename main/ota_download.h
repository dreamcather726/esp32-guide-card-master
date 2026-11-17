#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>

#include <http.h>

namespace ota_download {
constexpr size_t kBlock = 4096;
constexpr size_t kCheckpoint = 64 * 1024;
constexpr int kAttempts = 6;

struct Image {
    size_t size = 0;
    std::string etag;
    std::string sha256;
};

inline bool StrongEtag(const std::string& value) {
    if (value.size() < 2 || value.size() > 128 || value.front() != '"' || value.back() != '"') return false;
    return std::all_of(value.begin() + 1, value.end() - 1, [](unsigned char c) {
        return c >= 0x21 && c != '"' && c != 0x7f;
    });
}

inline bool HexDigest(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

inline bool Number(const std::string& text, size_t& pos, size_t& result) {
    result = 0;
    const size_t start = pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        const unsigned digit = text[pos++] - '0';
        if (result > (std::numeric_limits<size_t>::max() - digit) / 10) return false;
        result = result * 10 + digit;
    }
    return pos > start;
}

inline bool ContentRange(const std::string& value, size_t offset, size_t total) {
    if (value.compare(0, 6, "bytes ") != 0 || total == 0) return false;
    size_t pos = 6, first, last, size;
    return Number(value, pos, first) && pos < value.size() && value[pos++] == '-' &&
        Number(value, pos, last) && pos < value.size() && value[pos++] == '/' &&
        Number(value, pos, size) && pos == value.size() && first == offset &&
        size == total && last == total - 1 && first <= last;
}

// The sink owns flash/NVS transactions; checkpoints always end on erase boundaries.
class Sink {
public:
    virtual ~Sink() = default;
    virtual size_t Capacity() const = 0;
    virtual bool Load(Image& image, size_t& offset) = 0;
    virtual bool Clear() = 0;
    virtual bool Start(const Image& image, size_t offset) = 0;
    virtual bool Write(const char* data, size_t size) = 0;
    virtual bool Save(const Image& image, size_t offset) = 0;
    virtual void Abort() = 0;
    virtual bool Finish(const Image& image) = 0;
};

using Factory = std::function<std::unique_ptr<Http>()>;
using Progress = std::function<void(size_t, size_t)>;
using Retry = std::function<void(int, size_t)>;
using Log = std::function<void(const char*)>;

inline bool Run(const std::string& url, Sink& sink, Factory factory,
                Progress progress, Retry retry, Log log) {
    Image image;
    size_t offset = 0;
    if (!sink.Load(image, offset) || offset == 0 || offset % kBlock != 0 ||
        offset >= image.size || image.size > sink.Capacity() || !StrongEtag(image.etag)) {
        if (!sink.Clear()) return false;
        offset = 0;
    }
    auto buffer = std::make_unique<std::array<char, kBlock>>();
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (attempt) retry(attempt, offset);
        auto http = factory();
        if (!http) return false;
        http->SetTimeout(30000);
        http->SetKeepAlive(false);
        http->SetHeader("Accept-Encoding", "identity");
        if (offset) {
            http->SetHeader("Range", "bytes=" + std::to_string(offset) + "-");
            http->SetHeader("If-Range", image.etag);
        }
        if (!http->Open("GET", url)) {
            log("[OTA_RESUME] connection failed; retaining checkpoint");
            http->Close();
            continue;
        }
        const int status = http->GetStatusCode();
        if (offset && status == 416) {
            http->Close();
            if (!sink.Clear()) return false;
            offset = 0;
            log("[OTA_RESUME] range rejected; restarting from zero");
            continue;
        }
        if (status != 200 && status != 206) {
            http->Close();
            log("[OTA_RESUME] firmware HTTP status is not 200/206");
            if (status == 401 || status == 403 || status == 404) return false;
            continue;
        }
        const auto encoding = http->GetResponseHeader("Content-Encoding");
        const size_t length = http->GetBodyLength();
        const std::string etag = http->GetResponseHeader("ETag");
        const std::string hash = http->GetResponseHeader("X-Firmware-SHA256");
        if ((!encoding.empty() && encoding != "identity") ||
            (!hash.empty() && !HexDigest(hash))) {
            http->Close();
            log("[OTA_RESUME] invalid encoding or firmware digest");
            return false;
        }
        if (status == 206) {
            if (!offset || etag != image.etag || hash != image.sha256 ||
                length != image.size - offset ||
                !ContentRange(http->GetResponseHeader("Content-Range"), offset, image.size)) {
                http->Close();
                if (!sink.Clear()) return false;
                offset = 0;
                log("[OTA_RESUME] image/range changed; discarding checkpoint");
                continue;
            }
        } else {
            if (length < 288 || length > sink.Capacity()) {
                http->Close();
                log("[OTA_RESUME] invalid image length");
                return false;
            }
            if (!sink.Clear()) { http->Close(); return false; }
            if (offset) log("[OTA_RESUME] server returned 200; restarting from zero");
            offset = 0;
            image = {length, etag, hash};
        }
        if (!sink.Start(image, offset)) {
            http->Close();
            sink.Abort();
            if (!offset) return false;
            if (!sink.Clear()) return false;
            offset = 0;
            log("[OTA_RESUME] local prefix invalid; restarting from zero");
            continue;
        }
        size_t saved = offset;
        bool interrupted = false;
        while (offset < image.size) {
            const size_t wanted = std::min(kBlock, image.size - offset);
            size_t filled = 0;
            while (filled < wanted) {
                int n = http->Read(buffer->data() + filled, wanted - filled);
                if (n <= 0 || static_cast<size_t>(n) > wanted - filled) { interrupted = true; break; }
                filled += n;
            }
            if (interrupted) break;
            if (!sink.Write(buffer->data(), filled)) {
                http->Close();
                sink.Abort();
                sink.Clear();
                return false;
            }
            offset += filled;
            if (StrongEtag(image.etag) && offset < image.size && offset - saved >= kCheckpoint) {
                if (!sink.Save(image, offset)) { http->Close(); sink.Abort(); return false; }
                saved = offset;
            }
            progress(offset, image.size);
        }
        http->Close();
        if (!interrupted) {
            // No boot switch happens until the complete image has passed validation.
            if (sink.Finish(image)) { sink.Clear(); return true; }
            sink.Abort();
            sink.Clear();
            return false;
        }
        if (offset && StrongEtag(image.etag)) {
            if (offset != saved && !sink.Save(image, offset)) { sink.Abort(); return false; }
        } else {
            offset = 0;
        }
        sink.Abort();
        log("[OTA_RESUME] transfer interrupted; retrying from last complete flash block");
    }
    log("[OTA_RESUME] retry budget exhausted; checkpoint retained for next upgrade");
    return false;
}
}  // namespace ota_download
