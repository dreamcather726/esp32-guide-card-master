#include "ota_download.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

using namespace ota_download;
const std::string kData(150123, 'x');
const std::string kTag = "\"test-image\"";

struct Reply {
    int status = 200;
    bool open = true;
    size_t length = kData.size();
    std::string body = kData;
    size_t fragment = 512;
    std::map<std::string, std::string> headers{{"ETag", kTag}};
};
struct SinkFake : Sink {
    Image stored;
    size_t saved = 0;
    std::string flash;
    int starts = 0, aborts = 0, finishes = 0, commits = 0;
    bool save_ok = true, write_ok = true, finish_ok = true, prefix_ok = true, clear_ok = true;
    size_t Capacity() const override { return 4 * 1024 * 1024; }
    bool Load(Image& image, size_t& offset) override { image = stored; offset = saved; return saved > 0; }
    bool Clear() override { if (!clear_ok) return false; saved = 0; return true; }
    bool Start(const Image&, size_t offset) override {
        ++starts;
        if (offset && !prefix_ok) return false;
        assert(offset <= flash.size());
        flash.resize(offset);
        return true;
    }
    bool Write(const char* data, size_t size) override {
        if (!write_ok) return false;
        flash.append(data, size); return true;
    }
    bool Save(const Image& image, size_t offset) override {
        assert(offset % kBlock == 0 && offset == flash.size());
        if (!save_ok) return false;
        stored = image; saved = offset; ++commits; return true;
    }
    void Abort() override { ++aborts; }
    bool Finish(const Image& image) override { ++finishes; assert(flash.size() == image.size); return finish_ok; }
    void Pending(size_t offset = 65536) {
        stored = {kData.size(), kTag, ""}; saved = offset; flash = kData.substr(0, offset);
    }
};
struct HttpFake : Http {
    Reply reply;
    std::map<std::string, std::string>& request;
    size_t cursor = 0;
    HttpFake(Reply r, std::map<std::string, std::string>& h) : reply(std::move(r)), request(h) {}
    void SetTimeout(int n) override { assert(n == 30000); }
    void SetHeader(const std::string& k, const std::string& v) override { request[k] = v; }
    void SetContent(std::string&&) override {}
    void SetKeepAlive(bool b) override { assert(!b); }
    bool Open(const std::string&, const std::string&) override { return reply.open; }
    void Close() override {}
    int Read(char* buffer, size_t size) override {
        const size_t n = std::min({size, reply.fragment, reply.body.size() - cursor});
        if (!n) return reply.body.size() == reply.length ? 0 : -1;
        memcpy(buffer, reply.body.data() + cursor, n); cursor += n; return n;
    }
    int Write(const char*, size_t) override { return -1; }
    int GetStatusCode() override { return reply.status; }
    std::string GetResponseHeader(const std::string& key) const override {
        auto it = reply.headers.find(key); return it == reply.headers.end() ? "" : it->second;
    }
    size_t GetBodyLength() override { return reply.length; }
    std::string ReadAll() override { return reply.body; }
    int GetLastError() override { return -1; }
};
Reply Range(size_t offset) {
    Reply r;
    r.status = 206; r.length -= offset; r.body = kData.substr(offset);
    r.headers["Content-Range"] = "bytes " + std::to_string(offset) + "-" +
        std::to_string(kData.size() - 1) + "/" + std::to_string(kData.size());
    return r;
}
struct Test {
    SinkFake sink;
    std::vector<Reply> replies;
    std::vector<std::map<std::string, std::string>> requests;
    bool Run() {
        return ota_download::Run("https://test/app.bin", sink, [&]() -> std::unique_ptr<Http> {
            if (requests.size() >= replies.size()) return nullptr;
            const auto reply = replies[requests.size()];
            requests.emplace_back();
            return std::make_unique<HttpFake>(reply, requests.back());
        }, [](size_t a, size_t b) { assert(a <= b); }, [](int, size_t) {}, [](const char*) {});
    }
};

int main() {
    int cases = 0;
    { Test t; Reply r; r.fragment = 1; t.replies = {r}; assert(t.Run()); assert(t.sink.flash == kData); ++cases; }
    { Test t; Reply r; r.body.resize(147102); t.replies = {r, Range(143360)};
      assert(t.Run()); assert(t.sink.flash == kData); assert(t.requests[1]["Range"] == "bytes=143360-"); ++cases; }
    { Test t; t.sink.Pending(); t.replies = {Range(65536)}; assert(t.Run());
      assert(t.requests[0]["If-Range"] == kTag); assert(t.sink.flash == kData); ++cases; }
    { Test t; t.sink.Pending(); t.replies = {Reply{}}; assert(t.Run()); assert(t.sink.flash == kData); ++cases; }
    { Test t; t.sink.Pending(); Reply r; r.status = 416; t.replies = {r, Reply{}};
      assert(t.Run()); assert(!t.requests[1].count("Range")); ++cases; }
    for (const auto& invalid : {"bytes 0-150122/150123", "bytes 65536-150121/150123",
                               "bytes 65536-150122/999999", "bytes 65536-150122/*",
                               "bytes 65536-150122/150123 trailing", "bytes 999999999999999999999999999-150122/150123"}) {
        Test t; t.sink.Pending(); auto r = Range(65536); r.headers["Content-Range"] = invalid;
        t.replies = {r, Reply{}}; assert(t.Run()); assert(t.sink.starts == 1); ++cases;
    }
    { Test t; t.sink.Pending(); auto r = Range(65536); r.headers["ETag"] = "\"changed\"";
      t.replies = {r, Reply{}}; assert(t.Run()); assert(!t.requests[1].count("Range")); ++cases; }
    { Test t; t.sink.Pending(); auto r = Range(65536); r.length--; t.replies = {r, Reply{}};
      assert(t.Run()); assert(t.sink.starts == 1); ++cases; }
    { Test t; t.sink.Pending(); t.sink.prefix_ok = false; t.replies = {Range(65536), Reply{}};
      assert(t.Run()); assert(t.sink.flash == kData); ++cases; }
    { Test t; t.sink.Pending(7); t.replies = {Reply{}}; assert(t.Run()); assert(!t.requests[0].count("Range")); ++cases; }
    { Test t; Reply r; r.body.resize(80000); t.replies = {r}; t.sink.save_ok = false;
      assert(!t.Run()); assert(t.sink.finishes == 0 && t.sink.aborts == 1); ++cases; }
    { Test t; t.sink.write_ok = false; t.replies = {Reply{}};
      assert(!t.Run()); assert(t.sink.finishes == 0 && t.sink.aborts == 1); ++cases; }
    { Test t; t.sink.finish_ok = false; t.replies = {Reply{}};
      assert(!t.Run()); assert(t.sink.saved == 0 && t.sink.aborts == 1); ++cases; }
    { Test t; Reply r; r.open = false; t.replies.assign(kAttempts, r); t.sink.Pending();
      assert(!t.Run()); assert(t.sink.saved == 65536 && t.requests.size() == kAttempts); ++cases; }
    for (auto status : {401, 403, 404}) { Test t; Reply r; r.status = status; t.replies = {r};
      assert(!t.Run()); assert(t.sink.starts == 0); ++cases; }
    { Test t; Reply r; r.headers["ETag"] = "W/\"weak\""; r.body.resize(80000); t.replies = {r, Reply{}};
      assert(t.Run()); assert(!t.requests[1].count("Range")); ++cases; }
    { Test t; Reply r; r.headers["Content-Encoding"] = "gzip"; t.replies = {r};
      assert(!t.Run()); assert(t.sink.starts == 0); ++cases; }
    { Test t; Reply r; r.headers["X-Firmware-SHA256"] = "invalid"; t.replies = {r};
      assert(!t.Run()); assert(t.sink.starts == 0); ++cases; }
    for (auto size : {size_t(0), size_t(128), size_t(5000000)}) { Test t; Reply r; r.length = size; t.replies = {r};
      assert(!t.Run()); assert(t.sink.starts == 0); ++cases; }
    { Test t; t.sink.clear_ok = false; t.replies = {Reply{}}; assert(!t.Run()); assert(t.sink.starts == 0); ++cases; }
    { Test t; Reply r; r.body.resize(80000); t.replies = {r}; assert(!t.Run());
      assert(t.sink.saved == 77824); t.replies = {Range(77824)}; t.requests.clear();
      assert(t.Run()); assert(t.sink.flash == kData); ++cases; }
    std::cout << cases << " OTA transfer cases passed\n";
}
