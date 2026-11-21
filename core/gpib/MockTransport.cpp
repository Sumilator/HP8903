// SPDX-License-Identifier: GPL-2.0-only
#include "MockTransport.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <thread>

namespace gpib {

std::string MockTransport::formatHp(double value) {
    // The 8903 answers with a 5-digit mantissa: +00644E-03 = 0.644
    if (value == 0) return "+00000E+00\r\n";
    int exp = 0;
    double v = std::fabs(value);
    while (v < 10000) { v *= 10; --exp; }
    while (v >= 100000) { v /= 10; ++exp; }
    char s[32];
    std::snprintf(s, sizeof s, "%c%05ldE%+03d\r\n", value < 0 ? '-' : '+',
                  std::lround(v) > 99999 ? 99999L : std::lround(v), exp);
    return s;
}

std::vector<std::string> MockTransport::log() const {
    std::lock_guard<std::mutex> lk(m_);
    return log_;
}

void MockTransport::write(const std::string& cmd) {
    if (!open_) throw Error("mock: write on closed transport");
    std::lock_guard<std::mutex> lk(m_);
    log_.push_back(cmd);
    // Program strings may hold several codes: "M1 19.8SP", "H0L1".
    static const std::regex tok(
        R"((FR|AP)\s*([-+0-9.Ee]+)\s*(HZ|KZ|VL|MV|DV)|([0-9]+\.[0-9]+)\s*SP|[A-Z][0-9A-Z])",
        std::regex::icase);
    for (auto it = std::sregex_iterator(cmd.begin(), cmd.end(), tok);
         it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        if (m[1].matched) {
            double v = std::atof(m[2].str().c_str());
            std::string u = m[3].str();
            for (auto& c : u) c = char(std::toupper(c));
            if (std::toupper(m[1].str()[0]) == 'F')
                freq_ = (u == "KZ") ? v * 1e3 : v;
            else
                ampl_ = (u == "MV") ? v / 1e3 : (u == "DV") ? 0.7746 * std::pow(10.0, v / 20) : v;
        } else if (m[4].matched) {
            std::string sp = m[4].str();
            if (sp.rfind("19.", 0) == 0) loadOhm_ = std::atof(sp.c_str() + 3);
        } else {
            apply(m.str());
        }
    }
}

void MockTransport::apply(const std::string& c) {
    std::string code = c;
    for (auto& ch : code) ch = char(std::toupper(ch));
    if (code[0] == 'M' || code == "S1" || code == "S2" || code == "S3") {
        meas_ = code;
        if (code != "M1") loadOhm_ = 0;
    }
}

std::string MockTransport::read(double) {
    if (!open_) throw Error("mock: read on closed transport");
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    std::lock_guard<std::mutex> lk(m_);
    const double f = std::max(freq_, 1.0);
    const double hp = 1.0 / std::sqrt(1.0 + std::pow(20.0 / f, 2));
    const double lp = 1.0 / std::sqrt(1.0 + std::pow(f / 50000.0, 2));
    const double gain = 2.0;                       // a modest amplifier
    double vout = ampl_ * gain * hp * lp;
    const double clip = 3.2;                       // soft clip around 3 V
    double thdPct = 0.004 + 0.02 * std::pow(20.0 / f, 1.5) +
                    0.01 * std::pow(f / 20000.0, 2) +
                    (vout > 0 ? 0.002 / (vout + 1e-6) : 0);   // noise floor
    if (vout > clip) { thdPct += 8.0 * (vout - clip); vout = clip; }
    // A little measurement noise so traces look real.
    const double jitter = 1.0 + (std::rand() % 1000 - 500) * 2e-6;

    double value;
    if (meas_ == "M1") {
        value = vout * jitter;
        if (loadOhm_ > 0) value = vout * vout / loadOhm_;
    } else if (meas_ == "M3") {
        value = thdPct * jitter;
    } else if (meas_ == "M2") {
        value = -20 * std::log10(thdPct / 100);
    } else if (meas_ == "S2") {
        value = 90.0;
    } else if (meas_ == "S1") {
        value = 0.0012;
    } else {
        value = 1.0;
    }
    if (ampl_ <= 0 && meas_ == "M3") return "+90000E+05\r\n";   // error 31-ish
    return formatHp(value);
}

}  // namespace gpib
