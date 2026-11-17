#include "../main/boards/jiuchuan-s3/position_data.h"
#include <cassert>
#include <iostream>

int main() {
    NavigationStatus status;
    const std::string sample = R"({"UTC":"102205.000","latitude":"4003.829193","longitude":"11625.225067","hdop":"1.080000","altitude":"42.799999","fix":"1","cog":"146.080002","spkm":"0.642644","spkn":"0.347000","date":"171126","nsat":"15","dtype":"0","Wifisan":[],"IndoorID":"","IndoorX":"","IndoorY":""})";
    assert(position::Parse(sample, status));
    assert(status.satellites == 15 && status.has_fix && status.has_coordinates);
    assert(std::fabs(status.latitude - 40.0638198833333) < 1e-9);
    assert(std::fabs(status.longitude - 116.420417783333) < 1e-9);
    assert(status.scene.empty());
    assert(position::Parse(R"({"latitude":40.1,"longitude":116.2,"fix":1,"nsat":24,"Scene":"024812"})", status));
    assert(status.scene == "024812" && status.has_fix && status.satellites == 24);
    assert(position::Parse(R"({"payload":{"latitude":0,"longitude":116.2,"fix":1,"nsat":"0","scene":24812}})", status));
    assert(status.scene == "24812" && status.has_fix && status.satellites == 0);
    assert(position::Parse(R"({"latitude":"-4003.829193","longtitude":"-11625.225067","fix":"1","nsat":"15"})", status));
    assert(status.latitude < 0 && status.longitude < 0 && status.has_fix);
    for (const auto* bad : {"-1", "100", "15.5", "15junk", "NaN", "", "1e999"}) {
        const std::string json = std::string("{\"nsat\":\"") + bad + "\",\"fix\":0}";
        assert(position::Parse(json, status) && status.satellites == -1 && !status.has_fix);
    }
    assert(position::Parse(R"({"latitude":"4063.123","longitude":"11625.1","fix":1,"nsat":15})", status));
    assert(!status.has_fix && !status.has_coordinates);
    assert(position::Parse(R"({"latitude":"9001.1","longitude":"11625.1","fix":1})", status));
    assert(!status.has_fix);
    assert(position::Parse(R"({"latitude":"40junk","longitude":116,"fix":1})", status));
    assert(!status.has_fix);
    assert(position::Parse(R"({"latitude":40,"longitude":116,"fix":0,"nsat":9,"scene":""})", status));
    assert(!status.has_fix && status.satellites == 9 && status.scene.empty());
    assert(position::Parse(R"({"latitude":0,"longitude":0,"fix":1})", status) && !status.has_fix);
    assert(position::Parse(R"({"fix":1,"nsat":5,"Scene":"bad\nscene"})", status) && status.scene.empty());
    for (const auto* invalid : {"[]", "garbage", "{}", "{\"nsat\":15}trailing"})
        assert(!position::Parse(invalid, status));
    assert(!position::Parse(std::string(4097, 'x'), status));

    position::Message message;
    const std::string topic = "topic/864479086534998/position";
    auto feed = [&](const char* incoming, const std::string& bytes, int offset, int total, bool retained = false) {
        return message.Feed(topic, incoming, incoming ? static_cast<int>(std::strlen(incoming)) : 0,
                            bytes.data(), bytes.size(), offset, total, retained, status);
    };
    assert(!feed("topic/111111111111111/position", sample, 0, sample.size()));
    assert(!feed(topic.c_str(), sample, 0, sample.size(), true));
    assert(!feed(topic.c_str(), sample.substr(0, 20), 0, sample.size()));
    assert(feed(nullptr, sample.substr(20), 20, sample.size()) && status.satellites == 15);
    assert(!feed(topic.c_str(), sample.substr(0, 20), 0, sample.size()));
    assert(!feed(nullptr, sample.substr(21), 21, sample.size()));
    assert(!feed(topic.c_str(), sample, 0, 5000));
    assert(!feed(topic.c_str(), sample.substr(0, 20), 0, sample.size()));
    message.Reset();
    assert(!feed(nullptr, sample.substr(20), 20, sample.size()));
    assert(feed(topic.c_str(), sample, 0, sample.size()));

    for (int csq = -1; csq <= 100; ++csq) {
        const int expected = csq < 1 || csq > 31 ? 0 : csq > 27 ? 4 : csq >= 16 ? 3 : csq >= 8 ? 2 : 1;
        assert(position::SignalBars(csq) == expected);
    }
    assert(position::ParseCsq("\r\n+CSQ: 27,99\r\nOK\r\n") == 27);
    assert(position::ParseCsq("+CSQ: 99,99") == -1);
    assert(position::ParseCsq("+CSQ: 12junk") == -1);
    std::cout << "PASS ML307C sample, NMEA conversion, fix=1, satellite/scene validation, fragments, retained/foreign rejection and all CSQ boundaries\n";
}
