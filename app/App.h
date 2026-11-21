// SPDX-License-Identifier: GPL-2.0-only
//
// The HP 8903 application: UI state and the jobs it posts to the worker.
#pragma once

#include "Settings.h"
#include "Worker.h"

#include "gpib/Transport.h"
#include "hp8903/HP8903.h"
#include "hp8903/Sweep.h"

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

constexpr int kKinds = 5;    // SweepKind values
constexpr int kTraces = 4;

struct Trace {
    char name[32] = "";
    bool visible = true;
    hp8903::SweepResult result;
};

class App {
public:
    App();
    ~App();

    void frame();                  // draw one UI frame
    bool wantScreenshot() { bool w = screenshot_; screenshot_ = false; return w; }
    void screenshotSaved(const std::string& path);
    std::string screenshotPath() const;
    // Developer self-test: HP8903_SELFTEST=fr|thd|volt|power|monitor drives
    // the UI against the simulator, screenshots, then asks to quit.
    bool selfTestDone() const { return selfTestDone_; }

private:
    // UI sections
    void drawToolbar();
    void drawSweepTab(hp8903::SweepKind kind);
    void drawSweepSettings(hp8903::SweepKind kind);
    void drawPlot(hp8903::SweepKind kind);
    void drawMonitor();
    void drawConsole();
    void drawStatusBar();
    void drawInstrumentSettings();
    void drawFilterCombos(hp8903::LowPass& lp, hp8903::PlugIn& hp);

    // Jobs (run on the worker thread)
    void connect();
    void disconnect();
    void startSweep(hp8903::SweepKind kind, int trace);
    void stopAll();
    void startMonitor();
    void sendRaw(const std::string& codes, bool read);
    void exportCsv(hp8903::SweepKind kind);

    void log(const std::string& s);
    void refreshAdapters();
    void loadSettings();
    void saveSettings();
    static void reveal(const std::string& path);

    Settings settings_;
    std::unique_ptr<Worker> worker_ = std::make_unique<Worker>();

    // Connection (transport/analyzer are only touched on the worker thread,
    // except abort(), which is thread-safe).
    std::vector<gpib::AdapterInfo> adapters_;
    int adapterIndex_ = 0;
    int address_ = 28;
    std::unique_ptr<gpib::Transport> transport_;
    std::unique_ptr<hp8903::Analyzer> analyzer_;
    std::atomic<bool> connected_{false};
    std::string connectedTo_;

    // Sweeps
    std::array<hp8903::SweepConfig, kKinds> cfg_;
    std::array<std::array<Trace, kTraces>, kKinds> traces_;
    std::array<bool, kKinds> logY_{};
    bool frReuseRef_ = true;
    hp8903::SweepRunner runner_;
    std::atomic<bool> sweeping_{false};
    std::atomic<int> sweepKind_{-1}, sweepTrace_{-1};
    std::atomic<int> progress_{0}, progressTotal_{0};
    std::atomic<bool> fitPlot_{false};
    int powerMode_ = 0;   // 0 vs level, 1 vs frequency
    hp8903::Trigger trigger_ = hp8903::Trigger::FreeRun;
    double readTimeout_ = 15;

    // Monitor
    std::atomic<bool> monitoring_{false};
    struct MonitorCfg {
        double freq = 1000, level = 1.0;
        int meas = 0;   // index into kMonitorModes
        hp8903::LowPass lp = hp8903::LowPass::Off;
        hp8903::PlugIn hp = hp8903::PlugIn::Off;
        bool sourceOn = true;
    } mon_;
    std::atomic<int> monVersion_{0};
    std::string monReading_ = "-";
    std::vector<double> monHistory_;
    double monLast_ = 0;
    bool monValid_ = false;

    // Instrument naming (8903A: H1 = 400 Hz HP, H2 = optional weighting)
    char h1Name_[32] = "400 Hz HP";
    char h2Name_[32] = "A-weighting";
    char modelName_[16] = "8903A";
    bool showInstrumentSettings_ = false;

    // Console / log
    char consoleInput_[128] = "";
    std::deque<std::string> log_;
    std::string status_ = "Not connected";

    std::mutex data_;   // guards traces_, monitor results, log_, status_
    bool screenshot_ = false;
    void selfTestStep();
    std::string selfTest_;
    int selfTestFrame_ = 0, selfTestStage_ = 0;
    bool selfTestDone_ = false;
    int forceTab_ = -1;
};
