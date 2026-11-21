// SPDX-License-Identifier: GPL-2.0-only
//
// Sweep engine: the four measurement programs from Pete Millett's VEE
// software (as ported by pvdm), with a per-point callback for live plots.
#pragma once

#include "HP8903.h"

#include <atomic>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace hp8903 {

enum class SweepKind {
    FrequencyResponse,   // AC level vs f, dB relative to the level at refHz
    ThdVsFrequency,      // THD+N % and AC level vs f
    VoltageVsFrequency,  // AC level vs f, absolute Vrms
    PowerVsLevel,        // power (W) and THD+N vs source level, fixed f
    PowerVsFrequency,    // power (W) and THD+N vs f, fixed level
};

const char* name(SweepKind k);

struct SweepConfig {
    SweepKind kind = SweepKind::FrequencyResponse;
    double startHz = 20, stopHz = 20000;
    int points = 50;
    double levelV = 1.0;                       // source level (freq sweeps)
    double fixedHz = 1000;                     // power-vs-level frequency
    double startLevelV = 0.1, stopLevelV = 2;  // power-vs-level range
    double refHz = 1000;                       // frequency-response reference
    double loadOhm = 8;
    LowPass lowPass = LowPass::Off;
    PlugIn plugIn = PlugIn::Off;
    double settle = 0.3;          // seconds per point
    double lowFreqSettle = 0.8;   // used below 100 Hz instead, if larger
    double maxThdPct = 99;        // THD+N above this is plotted as a gap
    Trigger trigger = Trigger::FreeRun;
    double refLevelV = 0;         // >0: reuse a previous trace's reference
};

struct SweepPoint {
    double x = 0;           // Hz or Vrms
    double y1 = NAN;        // main value: dB, %, V or W
    double y2 = NAN;        // secondary: V (THD sweep) or THD+N % (power)
    double elapsed = 0;     // s spent on this point
    std::string note;       // error text if the point failed
};

struct SweepResult {
    SweepConfig config;
    std::vector<SweepPoint> points;
    double refLevelV = 0;
    bool cancelled = false;
    std::string error;
    std::string y1Label, y2Label, xLabel;
};

// Frequencies spaced evenly on a log scale, rounded to whole hertz.
std::vector<double> logSpace(double start, double stop, int n);
std::vector<double> linSpace(double start, double stop, int n);

class SweepRunner {
public:
    using PointFn = std::function<void(const SweepPoint&, int index, int total)>;
    using StatusFn = std::function<void(const std::string&)>;

    SweepResult run(Analyzer& a, const SweepConfig& cfg, PointFn onPoint = {},
                    StatusFn onStatus = {});
    void cancel() { cancel_ = true; }

private:
    std::atomic<bool> cancel_{false};
};

std::string toCsv(const std::vector<SweepResult>& traces,
                  const std::vector<std::string>& names);

}  // namespace hp8903
