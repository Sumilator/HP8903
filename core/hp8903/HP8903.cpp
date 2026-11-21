// SPDX-License-Identifier: GPL-2.0-only
#include "HP8903.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace hp8903 {

const char* code(Measurement m) {
    switch (m) {
        case Measurement::AcLevel: return "M1";
        case Measurement::Sinad: return "M2";
        case Measurement::Distortion: return "M3";
        case Measurement::DcLevel: return "S1";
        case Measurement::SignalToNoise: return "S2";
        case Measurement::DistortionLevel: return "S3";
    }
    return "M1";
}

const char* name(Measurement m) {
    switch (m) {
        case Measurement::AcLevel: return "AC level";
        case Measurement::Sinad: return "SINAD";
        case Measurement::Distortion: return "THD+N";
        case Measurement::DcLevel: return "DC level";
        case Measurement::SignalToNoise: return "Signal/noise";
        case Measurement::DistortionLevel: return "Distortion level";
    }
    return "";
}

const char* unit(Measurement m, bool log) {
    switch (m) {
        case Measurement::AcLevel: return log ? "dBm" : "V";
        case Measurement::Distortion: return log ? "dB" : "%";
        case Measurement::DistortionLevel: return log ? "dBm" : "V";
        case Measurement::DcLevel: return "V";
        default: return "dB";
    }
}

std::string Reading::describe() const {
    char s[64];
    if (valid) std::snprintf(s, sizeof s, "%.6g", value);
    else std::snprintf(s, sizeof s, "instrument error, raw %s", raw.c_str());
    return s;
}

Reading Analyzer::parse(const std::string& raw) {
    Reading r;
    // Trim CR/LF/spaces.
    size_t a = raw.find_first_not_of(" \r\n\t"), b = raw.find_last_not_of(" \r\n\t");
    r.raw = (a == std::string::npos) ? "" : raw.substr(a, b - a + 1);
    if (r.raw.empty()) return r;
    char* end = nullptr;
    double v = std::strtod(r.raw.c_str(), &end);
    if (end == r.raw.c_str()) return r;
    if (v >= 9e9) {
        // Not a measurement: the 8903 reports errors as values >= 9E9.
        // The raw text is kept so the user can look the code up.
        r.errorCode = -1;
        return r;
    }
    r.value = v;
    r.valid = true;
    return r;
}

void Analyzer::connect() {
    t_.open();
    t_.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    // Linear display, auto-range (both instrument defaults after clear,
    // set explicitly so a previous front-panel session can't surprise us).
    t_.write("LN");
}

void Analyzer::disconnect() {
    if (!t_.isOpen()) return;
    try { sourceOff(); } catch (...) {}
    t_.close();
}

void Analyzer::setFrequency(double hz) {
    // The 8903 does not accept exponent notation; whole hertz is what
    // Pete Millett's and pvdm's software send and what is known to work.
    char s[32];
    std::snprintf(s, sizeof s, "FR%ldHZ", std::lround(hz));
    t_.write(s);
}

void Analyzer::setAmplitude(double vrms) {
    char s[32];
    // Same format as pvdm's tested code: fixed-point volts, 5 decimals.
    if (vrms <= 0) std::snprintf(s, sizeof s, "AP0VL");
    else std::snprintf(s, sizeof s, "AP%.5fVL", vrms);
    t_.write(s);
}

void Analyzer::sourceOff() { t_.write("AP0VL"); }

void Analyzer::setFilters(LowPass lp, PlugIn hp) {
    std::string s;
    s += hp == PlugIn::Off ? "H0" : hp == PlugIn::Left ? "H1" : "H2";
    s += lp == LowPass::Off ? "L0" : lp == LowPass::LP30k ? "L1" : "L2";
    t_.write(s);
}

void Analyzer::setLog(bool log) { t_.write(log ? "LG" : "LN"); }

Reading Analyzer::readOne() {
    return parse(t_.read(readTimeout));
}

Reading Analyzer::measure(Measurement m, double settle) {
    if (settle > 0)
        std::this_thread::sleep_for(std::chrono::duration<double>(settle));
    std::string cmd = code(m);
    if (trigger_ == Trigger::Settled) cmd += "T3";
    t_.write(cmd);
    return readOne();
}

Reading Analyzer::measurePower(double loadOhm, double settle) {
    if (settle > 0)
        std::this_thread::sleep_for(std::chrono::duration<double>(settle));
    char s[48];
    std::snprintf(s, sizeof s, "M1 19.%gSP%s", loadOhm,
                  trigger_ == Trigger::Settled ? "T3" : "");
    t_.write(s);
    return readOne();
}

}  // namespace hp8903
