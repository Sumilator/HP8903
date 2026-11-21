// SPDX-License-Identifier: GPL-2.0-only
//
// HP 8903A / 8903B Audio Analyzer instrument layer.
//
// The 8903 predates IEEE 488.2: no *IDN?, no queries. You send front-panel
// "program codes" and, when addressed to talk, it returns the current
// reading of the right-hand display as ASCII, e.g. "+00644E-03".
//
// Program codes (HP 8903B Operation Manual; Keysight app note 5990-4135):
//   M1 AC level       M2 SINAD          M3 distortion (THD+N)
//   S1 DC level       S2 signal/noise   S3 distortion level
//   L0/L1/L2          low-pass off / 30 kHz / 80 kHz
//   H0/H1/H2          plug-in filter off / left slot / right slot
//   FRnnnHZ / KZ      source frequency
//   APnnnVL / MV / DV source amplitude (Vrms / mVrms / dBm 600 R)
//   LN / LG           linear / log (dB) display
//   T0..T3            free run / hold / trigger / trigger with settling
//   19.nSP            power units into n ohms (as used by Pete Millett's VEE)
// Readings >= 9E9 are error codes, not measurements.
#pragma once

#include "../gpib/Transport.h"

#include <string>

namespace hp8903 {

enum class Measurement { AcLevel, Sinad, Distortion, DcLevel, SignalToNoise, DistortionLevel };
enum class LowPass { Off, LP30k, LP80k };
enum class PlugIn { Off, Left, Right };
enum class Trigger { FreeRun, Settled };

const char* code(Measurement m);
const char* name(Measurement m);
const char* unit(Measurement m, bool log);

struct Reading {
    double value = 0;
    bool valid = false;
    int errorCode = 0;      // -1 when the instrument returned an error value
    std::string raw;
    std::string describe() const;
};

class Analyzer {
public:
    explicit Analyzer(gpib::Transport& t) : t_(t) {}

    void connect();      // open adapter, device clear, sane defaults
    void disconnect();   // source off, back to local, close

    void setFrequency(double hz);
    void setAmplitude(double vrms);
    void sourceOff();
    void setFilters(LowPass lp, PlugIn hp);
    void setLog(bool log);
    void setTrigger(Trigger t) { trigger_ = t; }

    // Select measurement, wait `settle` seconds, read the result.
    Reading measure(Measurement m, double settleSeconds);
    // AC level converted to watts into `loadOhm` by the instrument.
    Reading measurePower(double loadOhm, double settleSeconds);

    // Raw access for the console / experiments.
    void send(const std::string& codes) { t_.write(codes); }
    std::string readRaw(double timeout = 15.0) { return t_.read(timeout); }

    void abort() { t_.abort(); }
    gpib::Transport& transport() { return t_; }

    static Reading parse(const std::string& raw);

    double readTimeout = 15.0;   // a 20 Hz autoranging THD+N can take ~10 s

private:
    Reading readOne();
    gpib::Transport& t_;
    Trigger trigger_ = Trigger::FreeRun;
};

}  // namespace hp8903
