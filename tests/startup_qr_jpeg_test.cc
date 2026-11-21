#include "../main/boards/jiuchuan-s3/startup_qr_jpeg.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
int cases = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL " << message << '\n';
        std::exit(1);
    }
}

Bytes Frame(uint16_t width, uint16_t height, uint8_t marker = 0xc0) {
    return {0xff, marker, 0x00, 0x11, 8,
            static_cast<uint8_t>(height >> 8), static_cast<uint8_t>(height),
            static_cast<uint8_t>(width >> 8), static_cast<uint8_t>(width),
            3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1};
}

Bytes JpegHeader(uint16_t width, uint16_t height, uint8_t marker = 0xc0) {
    // Standard JFIF APP0, followed by DQT and a three-component SOF: the
    // layout of the 300x300 server image. No scan decoding is needed here.
    Bytes bytes = {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10,
                   'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0,
                   0xff, 0xdb, 0x00, 0x43, 0};
    bytes.insert(bytes.end(), 64, 8);
    const auto frame = Frame(width, height, marker);
    bytes.insert(bytes.end(), frame.begin(), frame.end());
    return bytes;
}

void Accept(const char* name, const Bytes& bytes, uint16_t width, uint16_t height) {
    startup_qr::JpegDimensions dimensions;
    Check(startup_qr::ReadJpegDimensions(bytes.data(), bytes.size(), dimensions), name);
    Check(dimensions.width == width && dimensions.height == height, "wrong dimensions");
    ++cases;
}

void Reject(const char* name, const Bytes& bytes) {
    startup_qr::JpegDimensions dimensions{123, 456};
    Check(!startup_qr::ReadJpegDimensions(bytes.data(), bytes.size(), dimensions), name);
    Check(dimensions.width == 0 && dimensions.height == 0, "failure retained stale dimensions");
    ++cases;
}
}  // namespace

int main() {
    Accept("baseline JFIF server dimensions", JpegHeader(300, 300), 300, 300);
    Accept("progressive dimensions recognized", JpegHeader(300, 300, 0xc2), 300, 300);
    Accept("smallest dimensions", JpegHeader(1, 1), 1, 1);
    Accept("maximum dimensions", JpegHeader(1024, 1024), 1024, 1024);
    Reject("zero width", JpegHeader(0, 300));
    Reject("zero height", JpegHeader(300, 0));
    Reject("oversized width", JpegHeader(1025, 300));
    Reject("oversized height", JpegHeader(300, 1025));
    Reject("tiny compressed header declaring huge image", JpegHeader(65535, 65535));
    Reject("empty file", {});
    Reject("missing SOI", {0xff, 0xe0, 0x00, 0x02});
    Reject("only SOI", {0xff, 0xd8});
    Reject("SOI and trailing FF", {0xff, 0xd8, 0xff});
    Reject("only repeated fill markers", {0xff, 0xd8, 0xff, 0xff});
    Reject("missing segment marker", {0xff, 0xd8, 0x01, 0x02});
    Reject("stuffed byte outside scan", {0xff, 0xd8, 0xff, 0x00});
    Reject("repeated SOI", {0xff, 0xd8, 0xff, 0xd8});
    Reject("restart before scan", {0xff, 0xd8, 0xff, 0xd0});
    Reject("EOI before frame", {0xff, 0xd8, 0xff, 0xd9});
    Reject("zero segment length", {0xff, 0xd8, 0xff, 0xe0, 0, 0});
    Reject("one byte segment length", {0xff, 0xd8, 0xff, 0xe0, 0, 1});
    Reject("segment extending beyond file", {0xff, 0xd8, 0xff, 0xe0, 0xff, 0xff});
    Reject("short SOF", {0xff, 0xd8, 0xff, 0xc0, 0, 2});

    auto valid = JpegHeader(300, 300);
    for (size_t end = 0; end < valid.size(); ++end) {
        Reject("header truncated at every byte", Bytes(valid.begin(), valid.begin() + end));
    }

    auto bad_count = valid;
    bad_count[bad_count.size() - 10] = 0;
    Reject("zero SOF components", bad_count);
    bad_count[bad_count.size() - 10] = 2;
    Reject("SOF length contradicts component count", bad_count);

    Bytes extras = {0xff, 0xd8, 0xff, 0xff, 0xe1, 0, 6, 0xff, 0xc0, 0xff, 0xda,
                    0xff, 0x01, 0xff, 0xc4, 0, 2, 0xff, 0xcc, 0, 2};
    const auto frame = Frame(300, 300);
    extras.insert(extras.end(), frame.begin(), frame.end());
    Accept("skip metadata and non-SOF C markers, accept fill and TEM", extras, 300, 300);

    Bytes early_scan = {0xff, 0xd8, 0xff, 0xda, 0, 2};
    early_scan.insert(early_scan.end(), frame.begin(), frame.end());
    Reject("never search scan data for SOF", early_scan);

    valid.insert(valid.end(), {0xff, 0xda, 0, 2, 0x12, 0xff, 0x00, 0x34, 0xff, 0xd9});
    Accept("header preflight leaves entropy validation to decoder", valid, 300, 300);

    startup_qr::JpegDimensions dimensions{123, 456};
    Check(!startup_qr::ReadJpegDimensions(nullptr, 100, dimensions), "null input");
    Check(dimensions.width == 0 && dimensions.height == 0, "null input retained dimensions");
    ++cases;
    std::cout << cases << " startup QR JPEG header checks passed\n";
}
