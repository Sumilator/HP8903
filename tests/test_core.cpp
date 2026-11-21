// SPDX-License-Identifier: GPL-2.0-only
// Unit tests: NI packet framing, HP reading parser, sweeps against the mock.
#include "gpib/MockTransport.h"
#include "gpib/NiUsbGpib.h"
#include "hp8903/HP8903.h"
#include "hp8903/Sweep.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using bytes = std::vector<uint8_t>;
using namespace gpib;

static void testFraming() {
    // Data write of "M1\n" with EOI, 3 s timeout: header, data, pad, term.
    const uint8_t d[] = {'M', '1', '\n'};
    bytes w = niproto::write(d, 3, true, niproto::timeoutCode(3e6));
    bytes want = {0x0d, 0xfd, 0xff, 0xfc, 0, 0, 0x08, 0, 'M', '1', '\n', 0, 0x04, 0, 0, 0};
    CHECK(w == want);

    // Command bytes UNL, LAD 28, MTA 0.
    const uint8_t c[] = {0x3f, 0x3c, 0x40};
    bytes cmd = niproto::command(c, 3, 0xfc);
    bytes wantc = {0x0c, 0xfd, 0x00, 0xfc, 0x3f, 0x3c, 0x40, 0x00, 0x04, 0, 0, 0};
    CHECK(cmd == wantc);

    // Register write program is 3-byte header + 3 bytes/reg, padded to 4.
    bytes r = niproto::regWrite({{1, 10, 0x1f}});
    bytes wantr = {0x09, 1, 0, 1, 10, 0x1f, 0, 0, 0x04, 0, 0, 0};
    CHECK(r == wantr);

    // Read 64 bytes: ~(63) = 0xffc0.
    bytes rd = niproto::read(64, 0xfe);
    CHECK(rd.size() == 24);
    CHECK(rd[0] == 0x0a && rd[3] == 0xfe && rd[4] == 0xc0 && rd[5] == 0xff);
    CHECK(rd[8] == 0x09 && rd[9] == 2 && rd[13] == 0x51 && rd[16] == 0x55);
    CHECK(rd[20] == 0x04);

    CHECK(niproto::timeoutCode(10e6) == 0xfd);
    CHECK(niproto::timeoutCode(15e6) == 0xfe);

    // Status count is two's complement: 0xfffe -> 2.
    const uint8_t st[] = {0x38, 0x20, 0x00, 0x00, 0xfe, 0xff, 0, 0};
    niproto::Status s = niproto::status(st);
    CHECK(s.id == 0x38 && s.ibsta == 0x2000 && s.count == 2);
}

static void testReadParse() {
    // Synthesised ibrd reply carrying "+00644E-03\r\n" (12 bytes) in one
    // 16-byte block, then the 12-byte ibrd status, reg-write status, term.
    bytes in = {0x36};
    const char* msg = "+00644E-03\r\n";
    for (int k = 0; k < 15; ++k) in.push_back(k < 12 ? uint8_t(msg[k]) : 0);
    bytes status = {0x38, 0x20, 0x00, 0x00, 0x00, 0x00, 0, 0, /*skip*/ 0, /*last*/ 12, 0, 0};
    in.insert(in.end(), status.begin(), status.end());
    bytes regst = {0x09, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0x04, 0, 0, 0};
    in.insert(in.end(), regst.begin(), regst.end());

    uint8_t buf[64] = {0};
    size_t n = 0;
    niproto::Status s{};
    CHECK(niproto::parseRead(in.data(), in.size(), buf, sizeof buf, &n, &s));
    CHECK(n == 12);
    CHECK(std::memcmp(buf, msg, 12) == 0);
    CHECK(s.ibsta & 0x2000);   // END

    CHECK(!niproto::parseRead(in.data(), 10, buf, sizeof buf, &n, &s));   // truncated
}

static void testHpParse() {
    using hp8903::Analyzer;
    auto r = Analyzer::parse("+00644E-03\r\n");
    CHECK(r.valid && std::fabs(r.value - 0.644) < 1e-12);
    r = Analyzer::parse("-12345E+00");
    CHECK(r.valid && r.value == -12345);
    r = Analyzer::parse("+90310E+05\r\n");
    CHECK(!r.valid && r.errorCode == -1 && r.raw == "+90310E+05");
    r = Analyzer::parse("");
    CHECK(!r.valid);
    CHECK(MockTransport::formatHp(0.644) == "+64400E-05\r\n");
}

static void testSweeps() {
    MockTransport t;
    hp8903::Analyzer a(t);
    a.connect();
    hp8903::SweepRunner run;

    hp8903::SweepConfig c;
    c.points = 12;
    c.settle = 0;
    c.lowFreqSettle = 0;
    auto r = run.run(a, c);
    CHECK(r.error.empty());
    CHECK(r.points.size() == 12);
    CHECK(r.refLevelV > 1.9 && r.refLevelV < 2.1);
    // 1 kHz is ~0 dB; 20 Hz is ~3 dB down in the mock's DUT.
    CHECK(std::fabs(r.points.front().y1 + 3.0) < 0.3);

    c.kind = hp8903::SweepKind::ThdVsFrequency;
    r = run.run(a, c);
    CHECK(r.error.empty() && !std::isnan(r.points[5].y1) && !std::isnan(r.points[5].y2));

    c.kind = hp8903::SweepKind::PowerVsLevel;
    c.startLevelV = 0.1; c.stopLevelV = 1.0; c.points = 5;
    r = run.run(a, c);
    CHECK(r.error.empty() && r.points.size() == 5);
    CHECK(r.points.back().y1 > r.points.front().y1);   // more level, more watts

    // The program codes reached the instrument as expected.
    bool sawFr = false, sawPower = false, sawOff = false;
    for (auto& s : t.log()) {
        if (s == "FR1000HZ") sawFr = true;
        if (s == "M1 19.8SP") sawPower = true;
        if (s == "AP0VL") sawOff = true;
    }
    CHECK(sawFr && sawPower && sawOff);

    c.kind = hp8903::SweepKind::FrequencyResponse;
    c.startHz = 5;   // out of range
    r = run.run(a, c);
    CHECK(!r.error.empty());

    auto ls = hp8903::logSpace(20, 20000, 4);
    CHECK(ls.size() == 4 && ls[0] == 20 && ls[1] == 200 && ls[3] == 20000);

    std::string csv = hp8903::toCsv({r}, {"L"});
    CHECK(csv.find("# HP 8903 sweep export") == 0);
}

int main() {
    testFraming();
    testReadParse();
    testHpParse();
    testSweeps();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::puts("All tests passed");
    return 0;
}
