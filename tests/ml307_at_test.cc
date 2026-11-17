#include "ml307_at.h"

#include <cassert>
#include <iostream>

int main() {
    using namespace ml307_at;
    const Interface normal[] = {
        {0, 0xe0, 1, 3, 1}, {1, 0x0a, 0, 0, 2},
        {2, 0xff, 0, 0, 2}, {4, 0xff, 0, 0, 2}, {8, 0xff, 0, 0, 2}
    };
    assert((ControlCandidates(normal, 5) == std::vector<uint8_t>{2, 8, 4}));
    const Interface excluded[] = {
        {2, 0xe0, 1, 3, 2}, {8, 2, 2, 0xff, 2}, {4, 0x0a, 0, 0, 2},
        {5, 8, 6, 0x50, 2}, {6, 0xff, 0, 0, 1}
    };
    assert(ControlCandidates(excluded, 5).empty());
    const Interface acm[] = {{0, 2, 2, 0, 1}, {1, 0x0a, 0, 0, 2}};
    assert((ControlCandidates(acm, 2) == std::vector<uint8_t>{0}));
    assert(ControlCandidates(acm, 1).empty());
    const Interface rndis_acm[] = {{0, 2, 2, 0xff, 1}, {1, 0x0a, 0, 0, 2}};
    assert(ControlCandidates(rndis_acm, 2).empty());
    const Interface duplicate[] = {{2, 0xff, 0, 0, 2}, {2, 0xff, 0, 0, 2}};
    assert((ControlCandidates(duplicate, 2) == std::vector<uint8_t>{2}));
    assert(ControlCandidates(nullptr, 0).empty());

    assert(IsImei("864479086486322"));
    assert(!IsImei("864479086486322s3"));
    assert(!IsImei("000000000000000"));
    assert(!IsImei("86447908648632"));
    assert(!IsImei("8644790864863221"));
    assert(!IsImei("864479O86486322"));
    assert(ExtractImei("AT+GSN\r\n864479086486322\r\nOK\r\n") == "864479086486322");
    assert(ExtractImei("\r\n+CGSN: \"864479086486322\"\r\nOK\r\n") == "864479086486322");
    assert(ExtractImei("+GSN: 864479086486322\nOK\n") == "864479086486322");
    assert(ExtractImei("+CGSN:864479086486322\r\n") == "864479086486322");
    assert(ExtractImei("\r\n+OTHER: 864479086486322\r\nOK\r\n").empty());
    assert(ExtractImei("ERROR\r\n").empty());
    assert(ExtractImei("8644790864863221\r\nOK\r\n").empty());
    assert(ExtractImei("000000000000000\r\nOK\r\n").empty());

    assert(ParseReply("\r\nOK\r\n") == Reply::Ok);
    assert(ParseReply("AT\r\nO") == Reply::Pending);
    assert(ParseReply("AT\r\nOK") == Reply::Pending);
    assert(ParseReply("+URC: BOOK\r\n") == Reply::Pending);
    assert(ParseReply("\r\nERROR\r\n") == Reply::Error);
    assert(ParseReply("+CME ERROR: 10\r\n") == Reply::Error);
    assert(ParseReply("+CMS ERROR: 500\n") == Reply::Error);
    assert(ParseReply("OK\r\nERROR\r\n") == Reply::Error);
    assert(RetryDelayMs(0) == 2000);
    assert(RetryDelayMs(1) == 4000);
    assert(RetryDelayMs(2) == 8000);
    assert(RetryDelayMs(3) == 16000);
    assert(RetryDelayMs(4) == 30000);
    assert(RetryDelayMs(100000) == 30000);
    std::cout << "ML307 AT helper: 35 checks passed\n";
}
