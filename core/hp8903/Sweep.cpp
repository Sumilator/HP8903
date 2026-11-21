// SPDX-License-Identifier: GPL-2.0-only
#include "Sweep.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace hp8903 {

const char* name(SweepKind k) {
    switch (k) {
        case SweepKind::FrequencyResponse: return "Frequency response";
        case SweepKind::ThdVsFrequency: return "THD+N vs frequency";
        case SweepKind::VoltageVsFrequency: return "Voltage vs frequency";
        case SweepKind::PowerVsLevel: return "Power vs level";
        case SweepKind::PowerVsFrequency: return "Power vs frequency";
    }
    return "";
}

std::vector<double> logSpace(double start, double stop, int n) {
    std::vector<double> v;
    if (n < 2) return {std::round(start)};
    const double a = std::log10(start), b = std::log10(stop);
    for (int i = 0; i < n; ++i) {
        double f = std::round(std::pow(10.0, a + (b - a) * i / (n - 1)));
        if (v.empty() || f != v.back()) v.push_back(f);   // drop duplicates
    }
    return v;
}

std::vector<double> linSpace(double start, double stop, int n) {
    std::vector<double> v;
    if (n < 2) return {start};
    for (int i = 0; i < n; ++i) v.push_back(start + (stop - start) * i / (n - 1));
    return v;
}

namespace {
double settleFor(const SweepConfig& c, double hz) {
    return hz < 100 ? std::max(c.settle, c.lowFreqSettle) : c.settle;
}
}  // namespace

SweepResult SweepRunner::run(Analyzer& a, const SweepConfig& cfg, PointFn onPoint,
                             StatusFn onStatus) {
    cancel_ = false;
    SweepResult r;
    r.config = cfg;
    auto status = [&](const std::string& s) { if (onStatus) onStatus(s); };
    using clock = std::chrono::steady_clock;

    const bool freqSweep = cfg.kind != SweepKind::PowerVsLevel;
    r.xLabel = freqSweep ? "Frequency (Hz)" : "Source level (Vrms)";
    switch (cfg.kind) {
        case SweepKind::FrequencyResponse: r.y1Label = "Level (dB)"; break;
        case SweepKind::ThdVsFrequency: r.y1Label = "THD+N (%)"; r.y2Label = "Level (Vrms)"; break;
        case SweepKind::VoltageVsFrequency: r.y1Label = "Level (Vrms)"; break;
        default: r.y1Label = "Power (W)"; r.y2Label = "THD+N (%)"; break;
    }

    try {
        if (freqSweep && (cfg.startHz < 20 || cfg.stopHz > 100000 || cfg.startHz >= cfg.stopHz))
            throw gpib::Error("Frequency range must lie within 20 Hz - 100 kHz, start below stop");
        a.setTrigger(cfg.trigger);
        a.setLog(false);
        a.setFilters(cfg.lowPass, cfg.plugIn);

        // Reference level for frequency response.
        if (cfg.kind == SweepKind::FrequencyResponse) {
            if (cfg.refLevelV > 0) {
                r.refLevelV = cfg.refLevelV;
            } else {
                status("Measuring reference at " + std::to_string(int(cfg.refHz)) + " Hz");
                a.setFrequency(cfg.refHz);
                a.setAmplitude(cfg.levelV);
                Reading ref = a.measure(Measurement::AcLevel, 1.0);
                if (!ref.valid || ref.value <= 0)
                    throw gpib::Error("Reference measurement failed: " + ref.describe() +
                                      " - check the signal path");
                r.refLevelV = ref.value;
            }
        }

        std::vector<double> xs = freqSweep ? logSpace(cfg.startHz, cfg.stopHz, cfg.points)
                                           : linSpace(cfg.startLevelV, cfg.stopLevelV, cfg.points);
        if (freqSweep) a.setAmplitude(cfg.levelV);
        else a.setFrequency(cfg.fixedHz);

        for (size_t i = 0; i < xs.size(); ++i) {
            if (cancel_) { r.cancelled = true; break; }
            SweepPoint p;
            p.x = xs[i];
            auto t0 = clock::now();
            const double hz = freqSweep ? p.x : cfg.fixedHz;
            if (freqSweep) a.setFrequency(p.x);
            else a.setAmplitude(p.x);
            const double st = settleFor(cfg, hz);

            auto measureWithRetry = [&](auto fn) {
                Reading rd;
                for (int attempt = 0; attempt < 2; ++attempt) {
                    try { rd = fn(); } catch (const gpib::Timeout& e) { rd = {}; rd.raw = e.what(); }
                    if (rd.valid || cancel_) break;
                }
                return rd;
            };

            Reading m1, m2;
            switch (cfg.kind) {
                case SweepKind::FrequencyResponse:
                    m1 = measureWithRetry([&] { return a.measure(Measurement::AcLevel, st); });
                    if (m1.valid && m1.value > 0) p.y1 = 20 * std::log10(m1.value / r.refLevelV);
                    break;
                case SweepKind::VoltageVsFrequency:
                    m1 = measureWithRetry([&] { return a.measure(Measurement::AcLevel, st); });
                    if (m1.valid) p.y1 = m1.value;
                    break;
                case SweepKind::ThdVsFrequency:
                    m1 = measureWithRetry([&] { return a.measure(Measurement::Distortion, st); });
                    m2 = measureWithRetry([&] { return a.measure(Measurement::AcLevel, 0); });
                    if (m1.valid && m1.value <= cfg.maxThdPct) p.y1 = m1.value;
                    if (m2.valid) p.y2 = m2.value;
                    break;
                case SweepKind::PowerVsLevel:
                case SweepKind::PowerVsFrequency:
                    m2 = measureWithRetry([&] { return a.measure(Measurement::Distortion, st); });
                    m1 = measureWithRetry([&] { return a.measurePower(cfg.loadOhm, 0); });
                    if (m1.valid) p.y1 = m1.value;
                    if (m2.valid && m2.value <= cfg.maxThdPct) p.y2 = m2.value;
                    break;
            }
            if (!m1.valid && !m1.raw.empty()) p.note = m1.describe();
            p.elapsed = std::chrono::duration<double>(clock::now() - t0).count();
            r.points.push_back(p);
            if (onPoint) onPoint(p, int(i), int(xs.size()));
        }
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    try { a.sourceOff(); } catch (...) {}
    return r;
}

std::string toCsv(const std::vector<SweepResult>& traces,
                  const std::vector<std::string>& names) {
    std::ostringstream o;
    o << "# HP 8903 sweep export\n";
    for (size_t t = 0; t < traces.size(); ++t) {
        const auto& r = traces[t];
        const auto& c = r.config;
        o << "# Trace " << (t < names.size() ? names[t] : std::to_string(t + 1)) << ": "
          << name(c.kind) << ", level " << c.levelV << " Vrms";
        if (r.refLevelV > 0) o << ", reference " << r.refLevelV << " Vrms @ " << c.refHz << " Hz";
        if (c.kind == SweepKind::PowerVsLevel || c.kind == SweepKind::PowerVsFrequency)
            o << ", load " << c.loadOhm << " ohm";
        o << "\n";
    }
    o << "trace," << (traces.empty() ? "x" : traces[0].xLabel) << ",y1,y2,label_y1,label_y2,seconds,note\n";
    char buf[64];
    for (size_t t = 0; t < traces.size(); ++t) {
        const auto& r = traces[t];
        const std::string nm = t < names.size() ? names[t] : std::to_string(t + 1);
        for (const auto& p : r.points) {
            o << nm << ',';
            std::snprintf(buf, sizeof buf, "%.6g", p.x); o << buf << ',';
            if (!std::isnan(p.y1)) { std::snprintf(buf, sizeof buf, "%.6g", p.y1); o << buf; }
            o << ',';
            if (!std::isnan(p.y2)) { std::snprintf(buf, sizeof buf, "%.6g", p.y2); o << buf; }
            std::snprintf(buf, sizeof buf, "%.3f", p.elapsed);
            o << ',' << r.y1Label << ',' << r.y2Label << ',' << buf << ',' << p.note << '\n';
        }
    }
    return o.str();
}

}  // namespace hp8903
