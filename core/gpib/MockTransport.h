// SPDX-License-Identifier: GPL-2.0-only
//
// Simulated HP 8903 for developing and demonstrating without hardware.
// Models a DUT with a gentle 20 Hz high-pass / 50 kHz low-pass response
// and distortion that rises at the frequency extremes and with level.
#pragma once

#include "Transport.h"

#include <mutex>
#include <string>
#include <vector>

namespace gpib {

class MockTransport : public Transport {
public:
    explicit MockTransport(int address = 28) { address_ = address; }

    void open() override { open_ = true; }
    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }
    void clear() override {}
    void write(const std::string& cmd) override;
    std::string read(double timeoutSeconds) override;
    void goToLocal() override {}
    std::string describe() const override { return "Simulated HP 8903 (no hardware)"; }

    // Every program string received, for tests.
    std::vector<std::string> log() const;

    static std::string formatHp(double v);   // "+00644E-03" style

private:
    void apply(const std::string& code);
    mutable std::mutex m_;
    bool open_ = false;
    double freq_ = 1000.0;
    double ampl_ = 0.0;      // Vrms
    std::string meas_ = "M1";
    double loadOhm_ = 0;     // >0 when 19.xSP power units active
    std::vector<std::string> log_;
};

}  // namespace gpib
