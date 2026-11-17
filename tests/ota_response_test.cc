#include "ota_response.h"
#include <cassert>
#include <iostream>

int main() {
    int checks = 0;
    for (const char* invalid : {"null", "[]", "{}", "{\"code\":500,\"msg\":\"server error\"}",
            "{\"error\":\"Internal Server Error\"}", "{\"websocket\":{}}",
            "{\"websocket\":{\"url\":\"\"}}", "{\"mqtt\":{\"endpoint\":null}}",
            "{\"activation\":{\"message\":\"wait\"}}", "{\"server_time\":{\"timestamp\":123}}",
            "{\"firmware\":{\"version\":\"2.1.12\",\"url\":\"https://example.test/fw.bin\"}}",
            "{\"code\":403,\"websocket\":{\"url\":\"wss://example.test\"}}",
            "{\"success\":false,\"websocket\":{\"url\":\"wss://example.test\"}}"}) {
        auto root = cJSON_Parse(invalid);
        assert(!ota_response::IsConfiguration(root));
        cJSON_Delete(root); ++checks;
    }
    for (const char* valid : {"{\"activation\":{\"code\":\"123456\"}}",
            "{\"activation\":{\"challenge\":\"challenge\"}}",
            "{\"websocket\":{\"url\":\"wss://example.test\"}}",
            "{\"mqtt\":{\"endpoint\":\"example.test:8883\"}}",
            "{\"code\":0,\"websocket\":{\"url\":\"wss://example.test\"}}"}) {
        auto root = cJSON_Parse(valid);
        assert(ota_response::IsConfiguration(root));
        cJSON_Delete(root); ++checks;
    }
    for (const char* invalid : {"", " ", "latest", "v2.1.11", "2..1", "2.1.", "2.1.-1",
            "2.1.11-", "2.1.11+", "2.1.11??", "999999999999.1.0"}) {
        assert(ota_response::ParseVersion(invalid).empty());
        assert(!ota_response::IsNewer(invalid, "2.1.12"));
        assert(!ota_response::IsNewer("2.1.11", invalid)); ++checks;
    }
    assert(!ota_response::IsNewer("2.1.11-diag", "2.1.10"));
    assert(!ota_response::IsNewer("2.1.11-diag", "2.1.11"));
    assert(ota_response::IsNewer("2.1.11-diag", "2.1.12"));
    assert(!ota_response::IsNewer("2.1.11-diag.2+build.123", "2.1.11+release"));
    assert(!ota_response::IsNewer("2.1", "2.1.0"));
    assert(ota_response::IsNewer("2.1.9", "2.1.10"));
    assert(ota_response::IsNewer("2.1.0", "2.1.0.1"));
    std::cout << "OTA response/version: " << checks + 7 << " checks passed\n";
}
