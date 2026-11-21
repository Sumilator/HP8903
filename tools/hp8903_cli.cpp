// SPDX-License-Identifier: GPL-2.0-only
//
// hp8903-cli: command-line tester for the GPIB driver and the HP 8903 layer.
// The first thing to run on new hardware, before the GUI.
//
//   hp8903-cli list                      adapters attached
//   hp8903-cli probe                     open the NI adapter, IFC, report status
//   hp8903-cli read                      one AC-level reading
//   hp8903-cli send "FR1000HZAP1VLM1" [--read]
//   hp8903-cli measure --freq 1000 --level 1 --mode thd
//   hp8903-cli sweep fr|thd|volt|plevel|pfreq [options] [--csv file]
//
// Common options: --adapter ni-usb|mock   --addr 28   --verbose

#include "../core/gpib/NiUsbGpib.h"
#include "../core/gpib/Transport.h"
#include "../core/hp8903/HP8903.h"
#include "../core/hp8903/Sweep.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace hp8903;

namespace {

struct Args {
    std::string cmd, sub;
    std::map<std::string, std::string> opt;
    std::vector<std::string> pos;
    double num(const std::string& k, double d) const {
        auto it = opt.find(k);
        return it == opt.end() ? d : std::atof(it->second.c_str());
    }
    std::string str(const std::string& k, const std::string& d) const {
        auto it = opt.find(k);
        return it == opt.end() ? d : it->second;
    }
    bool flag(const std::string& k) const { return opt.count(k) > 0; }
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            std::string k = s.substr(2);
            if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0 &&
                k != "read" && k != "verbose")
                a.opt[k] = argv[++i];
            else
                a.opt[k] = "1";
        } else if (a.cmd.empty()) a.cmd = s;
        else a.pos.push_back(s);
    }
    return a;
}

int usage() {
    std::puts(
        "hp8903-cli - HP 8903 audio analyzer over GPIB\n\n"
        "  list                         show attached adapters\n"
        "  probe                        open NI adapter, pulse IFC, show status\n"
        "  read                         one AC level reading\n"
        "  send \"<codes>\" [--read]      send raw program codes\n"
        "  measure [--freq 1000] [--level 1] [--mode ac|thd|sinad|snr|dc]\n"
        "  sweep fr|thd|volt|plevel|pfreq [--start 20] [--stop 20000]\n"
        "        [--points 30] [--level 1] [--freq 1000] [--load 8]\n"
        "        [--lp off|30k|80k] [--plugin off|left|right] [--settle 0.3]\n"
        "        [--settled] [--csv out.csv]\n\n"
        "  --adapter ni-usb|mock   (default ni-usb)\n"
        "  --addr N                GPIB address (default 28)");
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    Args a = parse(argc, argv);
    if (a.cmd.empty() || a.cmd == "help") return usage();

    try {
        if (a.cmd == "list") {
            for (auto& ad : gpib::listAdapters())
                std::printf("%-8s %-40s open as: --adapter %s\n", ad.kind.c_str(),
                            ad.name.c_str(), ad.id.c_str());
            return 0;
        }

        if (a.cmd == "probe") {
            gpib::NiUsbBoard b;
            b.open(a.str("serial", ""));
            std::printf("Opened %s, board serial %08X\n", b.productName().c_str(),
                        b.boardSerial());
            b.interfaceClear();
            b.remoteEnable(true);
            unsigned st = b.status();
            std::printf("Status 0x%04x:%s%s%s%s\n", st,
                        st & gpib::NiUsbBoard::CIC ? " CIC" : "",
                        st & gpib::NiUsbBoard::ATN ? " ATN" : "",
                        st & gpib::NiUsbBoard::REM ? " REM" : "",
                        st & gpib::NiUsbBoard::SRQI ? " SRQ" : "");
            std::puts(st & gpib::NiUsbBoard::CIC
                          ? "Adapter is controller-in-charge. Driver OK."
                          : "Warning: adapter did not report CIC.");
            b.remoteEnable(false);
            return 0;
        }

        const int addr = int(a.num("addr", 28));
        auto t = gpib::makeTransport(a.str("adapter", "ni-usb"), addr);
        Analyzer an(*t);
        an.connect();
        std::printf("Connected: %s\n", t->describe().c_str());

        int rc = 0;
        if (a.cmd == "read") {
            Reading r = an.measure(Measurement::AcLevel, 0.3);
            std::printf("AC level: %s V\n", r.describe().c_str());
        } else if (a.cmd == "send") {
            if (a.pos.empty()) return usage();
            an.send(a.pos[0]);
            if (a.flag("read")) std::printf("<< %s\n", an.readRaw().c_str());
        } else if (a.cmd == "measure") {
            std::string mode = a.str("mode", "ac");
            Measurement m = mode == "thd" ? Measurement::Distortion
                          : mode == "sinad" ? Measurement::Sinad
                          : mode == "snr" ? Measurement::SignalToNoise
                          : mode == "dc" ? Measurement::DcLevel
                          : Measurement::AcLevel;
            an.setFrequency(a.num("freq", 1000));
            an.setAmplitude(a.num("level", 1.0));
            Reading r = an.measure(m, 1.0);
            std::printf("%s: %s %s\n", name(m), r.describe().c_str(), unit(m, false));
            an.sourceOff();
        } else if (a.cmd == "sweep") {
            if (a.pos.empty()) return usage();
            SweepConfig c;
            std::string k = a.pos[0];
            c.kind = k == "thd" ? SweepKind::ThdVsFrequency
                   : k == "volt" ? SweepKind::VoltageVsFrequency
                   : k == "plevel" ? SweepKind::PowerVsLevel
                   : k == "pfreq" ? SweepKind::PowerVsFrequency
                   : SweepKind::FrequencyResponse;
            c.startHz = a.num("start", 20);
            c.stopHz = a.num("stop", 20000);
            c.points = int(a.num("points", 30));
            c.levelV = a.num("level", 1.0);
            c.fixedHz = a.num("freq", 1000);
            c.startLevelV = a.num("start-level", 0.1);
            c.stopLevelV = a.num("stop-level", 2.0);
            c.loadOhm = a.num("load", 8);
            c.settle = a.num("settle", 0.3);
            std::string lp = a.str("lp", "off"), pi = a.str("plugin", "off");
            c.lowPass = lp == "30k" ? LowPass::LP30k : lp == "80k" ? LowPass::LP80k : LowPass::Off;
            c.plugIn = pi == "left" ? PlugIn::Left : pi == "right" ? PlugIn::Right : PlugIn::Off;
            if (a.flag("settled")) c.trigger = Trigger::Settled;

            SweepRunner runner;
            std::printf("%s\n", name(c.kind));
            SweepResult res = runner.run(
                an, c,
                [&](const SweepPoint& p, int i, int n) {
                    std::printf("  [%2d/%2d] %10.4g  %12.6g  %12.6g  %.2fs %s\n", i + 1, n,
                                p.x, p.y1, p.y2, p.elapsed, p.note.c_str());
                    std::fflush(stdout);
                },
                [](const std::string& s) { std::printf("  %s\n", s.c_str()); });
            if (res.refLevelV > 0) std::printf("Reference level: %.6g Vrms\n", res.refLevelV);
            if (!res.error.empty()) { std::fprintf(stderr, "Error: %s\n", res.error.c_str()); rc = 2; }
            if (a.flag("csv")) {
                std::ofstream(a.str("csv", "sweep.csv")) << toCsv({res}, {"1"});
                std::printf("Saved %s\n", a.str("csv", "sweep.csv").c_str());
            }
        } else {
            rc = usage();
        }
        an.disconnect();
        return rc;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 2;
    }
}
