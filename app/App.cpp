// SPDX-License-Identifier: GPL-2.0-only
#include "App.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

using namespace hp8903;

namespace {

const ImVec4 kTraceColors[kTraces] = {
    {0.98f, 0.76f, 0.20f, 1}, {0.30f, 0.75f, 1.00f, 1},
    {0.55f, 0.90f, 0.45f, 1}, {0.95f, 0.45f, 0.65f, 1}};

const char* kKindTab[kKinds] = {"Frequency response", "THD+N", "Voltage",
                                "Power", "Power"};

struct MonitorMode { const char* label; Measurement m; };
const MonitorMode kMonitorModes[] = {
    {"AC level", Measurement::AcLevel},
    {"THD+N", Measurement::Distortion},
    {"SINAD", Measurement::Sinad},
    {"Signal/noise", Measurement::SignalToNoise},
    {"Distortion level", Measurement::DistortionLevel},
    {"DC level", Measurement::DcLevel}};

std::string nowStamp() {
    std::time_t t = std::time(nullptr);
    char b[32];
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", std::localtime(&t));
    return b;
}

std::string fmtEng(double v, const char* unit) {
    char b[48];
    double a = std::fabs(v);
    if (std::string(unit) == "V") {
        if (a < 1e-3) std::snprintf(b, sizeof b, "%.3f uV", v * 1e6);
        else if (a < 1) std::snprintf(b, sizeof b, "%.3f mV", v * 1e3);
        else std::snprintf(b, sizeof b, "%.4f V", v);
    } else if (std::string(unit) == "%") {
        std::snprintf(b, sizeof b, a < 0.01 ? "%.5f %%" : "%.4f %%", v);
    } else {
        std::snprintf(b, sizeof b, "%.2f %s", v, unit);
    }
    return b;
}

void helpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

}  // namespace

// ===========================================================================

App::App() {
    for (int k = 0; k < kKinds; ++k) {
        cfg_[k].kind = SweepKind(k);
        for (int t = 0; t < kTraces; ++t) std::snprintf(traces_[k][t].name, 32, "TR%d", t + 1);
    }
    cfg_[int(SweepKind::FrequencyResponse)].points = 50;
    cfg_[int(SweepKind::ThdVsFrequency)].points = 25;
    cfg_[int(SweepKind::VoltageVsFrequency)].points = 50;
    cfg_[int(SweepKind::PowerVsLevel)].points = 20;
    cfg_[int(SweepKind::PowerVsFrequency)].points = 25;
    logY_[int(SweepKind::ThdVsFrequency)] = true;
    logY_[int(SweepKind::PowerVsLevel)] = true;
    logY_[int(SweepKind::PowerVsFrequency)] = true;
    loadSettings();
    refreshAdapters();
    log("HP 8903 ready. Choose an adapter and press Connect.");
    if (const char* st = std::getenv("HP8903_SELFTEST")) selfTest_ = st;
}

void App::selfTestStep() {
    ++selfTestFrame_;
    auto kindFor = [&] {
        if (selfTest_ == "thd") return SweepKind::ThdVsFrequency;
        if (selfTest_ == "volt") return SweepKind::VoltageVsFrequency;
        if (selfTest_ == "power") return SweepKind::PowerVsLevel;
        return SweepKind::FrequencyResponse;
    };
    static const char* tabs[] = {"fr", "thd", "volt", "power", "monitor"};
    for (int i = 0; i < 5; ++i) if (selfTest_ == tabs[i]) forceTab_ = i;
    switch (selfTestStage_) {
        case 0:
            for (size_t i = 0; i < adapters_.size(); ++i)
                if (adapters_[i].kind == "mock") adapterIndex_ = int(i);
            connect();
            selfTestStage_ = 1;
            break;
        case 1:
            if (connected_ && !worker_->busy()) {
                if (selfTest_ == "monitor") {
                    mon_.meas = 1;
                    startMonitor();
                    selfTestStage_ = 4;
                    selfTestFrame_ = 0;
                    break;
                }
                auto& c = cfg_[int(kindFor())];
                c.settle = 0; c.lowFreqSettle = 0; c.points = 40;
                startSweep(kindFor(), 0);
                selfTestStage_ = 2;
            }
            break;
        case 2:
            if (!worker_->busy()) {
                auto& c = cfg_[int(kindFor())];
                c.levelV = 1.4; c.startLevelV = 0.2; c.stopLevelV = 2.0;
                std::snprintf(traces_[int(kindFor())][1].name, 32, "Right");
                std::snprintf(traces_[int(kindFor())][0].name, 32, "Left");
                startSweep(kindFor(), 1);
                selfTestStage_ = 3;
                selfTestFrame_ = 0;
            }
            break;
        case 3:
            if (!worker_->busy() && selfTestFrame_ > 10) { screenshot_ = true; selfTestStage_ = 5; }
            break;
        case 4:
            if (selfTestFrame_ > 150) { screenshot_ = true; selfTestStage_ = 5; }
            break;
        case 5:
            selfTestDone_ = true;
            monitoring_ = false;
            break;
    }
}

App::~App() {
    runner_.cancel();
    monitoring_ = false;
    saveSettings();
    if (sweeping_ && transport_) transport_->abort();
    worker_->post([this] {
        if (analyzer_) {
            try { analyzer_->disconnect(); } catch (...) {}
        }
    });
    worker_.reset();   // drain the queue and join before members go away
}

void App::log(const std::string& s) {
    std::lock_guard<std::mutex> lk(data_);
    char t[16];
    std::time_t now = std::time(nullptr);
    std::strftime(t, sizeof t, "%H:%M:%S ", std::localtime(&now));
    log_.push_back(t + s);
    if (log_.size() > 500) log_.pop_front();
    status_ = s;
}

void App::refreshAdapters() {
    std::string prev = adapters_.empty() ? settings_.get("adapter", "ni-usb")
                                         : adapters_[adapterIndex_].id;
    adapters_ = gpib::listAdapters();
    adapterIndex_ = 0;
    for (size_t i = 0; i < adapters_.size(); ++i)
        if (adapters_[i].id == prev) adapterIndex_ = int(i);
}

// ===========================================================================
// Settings
// ===========================================================================

void App::loadSettings() {
    settings_.load();
    address_ = int(settings_.num("address", 28));
    readTimeout_ = settings_.num("timeout", 15);
    trigger_ = settings_.num("settledTrigger", 0) > 0 ? Trigger::Settled : Trigger::FreeRun;
    frReuseRef_ = settings_.num("frReuseRef", 1) > 0;
    std::snprintf(h1Name_, sizeof h1Name_, "%s", settings_.get("h1Name", "400 Hz HP").c_str());
    std::snprintf(h2Name_, sizeof h2Name_, "%s", settings_.get("h2Name", "A-weighting").c_str());
    std::snprintf(modelName_, sizeof modelName_, "%s", settings_.get("model", "8903A").c_str());
    for (int k = 0; k < kKinds; ++k) {
        auto& c = cfg_[k];
        std::string p = "k" + std::to_string(k) + ".";
        c.startHz = settings_.num(p + "start", c.startHz);
        c.stopHz = settings_.num(p + "stop", c.stopHz);
        c.points = int(settings_.num(p + "points", c.points));
        c.levelV = settings_.num(p + "level", c.levelV);
        c.fixedHz = settings_.num(p + "fixedHz", c.fixedHz);
        c.startLevelV = settings_.num(p + "startLevel", c.startLevelV);
        c.stopLevelV = settings_.num(p + "stopLevel", c.stopLevelV);
        c.refHz = settings_.num(p + "refHz", c.refHz);
        c.loadOhm = settings_.num(p + "load", c.loadOhm);
        c.lowPass = LowPass(int(settings_.num(p + "lp", 0)));
        c.plugIn = PlugIn(int(settings_.num(p + "hp", 0)));
        c.settle = settings_.num(p + "settle", c.settle);
        c.lowFreqSettle = settings_.num(p + "lfSettle", c.lowFreqSettle);
        logY_[k] = settings_.num(p + "logY", logY_[k]) > 0;
    }
}

void App::saveSettings() {
    if (!adapters_.empty()) settings_.set("adapter", adapters_[adapterIndex_].id);
    settings_.set("address", address_);
    settings_.set("timeout", readTimeout_);
    settings_.set("settledTrigger", trigger_ == Trigger::Settled ? 1.0 : 0.0);
    settings_.set("frReuseRef", frReuseRef_ ? 1.0 : 0.0);
    settings_.set("h1Name", std::string(h1Name_));
    settings_.set("h2Name", std::string(h2Name_));
    settings_.set("model", std::string(modelName_));
    for (int k = 0; k < kKinds; ++k) {
        const auto& c = cfg_[k];
        std::string p = "k" + std::to_string(k) + ".";
        settings_.set(p + "start", c.startHz);
        settings_.set(p + "stop", c.stopHz);
        settings_.set(p + "points", double(c.points));
        settings_.set(p + "level", c.levelV);
        settings_.set(p + "fixedHz", c.fixedHz);
        settings_.set(p + "startLevel", c.startLevelV);
        settings_.set(p + "stopLevel", c.stopLevelV);
        settings_.set(p + "refHz", c.refHz);
        settings_.set(p + "load", c.loadOhm);
        settings_.set(p + "lp", double(int(c.lowPass)));
        settings_.set(p + "hp", double(int(c.plugIn)));
        settings_.set(p + "settle", c.settle);
        settings_.set(p + "lfSettle", c.lowFreqSettle);
        settings_.set(p + "logY", logY_[k] ? 1.0 : 0.0);
    }
    settings_.save();
}

// ===========================================================================
// Jobs
// ===========================================================================

void App::connect() {
    if (adapters_.empty()) return;
    const std::string spec = adapters_[adapterIndex_].id;
    const int addr = address_;
    const double tmo = readTimeout_;
    saveSettings();
    log("Connecting via " + adapters_[adapterIndex_].name + " ...");
    worker_->post([this, spec, addr, tmo] {
        try {
            transport_ = gpib::makeTransport(spec, addr);
            analyzer_ = std::make_unique<Analyzer>(*transport_);
            analyzer_->readTimeout = tmo;
            analyzer_->connect();
            // Prove the instrument is there: one AC-level reading.
            Reading r = analyzer_->measure(Measurement::AcLevel, 0.2);
            connectedTo_ = transport_->describe();
            connected_ = true;
            log("Connected: " + connectedTo_ + "  (AC level " +
                (r.valid ? fmtEng(r.value, "V") : r.describe()) + ")");
        } catch (const std::exception& e) {
            try { if (transport_) transport_->close(); } catch (...) {}
            analyzer_.reset();
            transport_.reset();
            connected_ = false;
            log(std::string("Connect failed: ") + e.what());
        }
    });
}

void App::disconnect() {
    stopAll();
    worker_->post([this] {
        try { if (analyzer_) analyzer_->disconnect(); } catch (...) {}
        analyzer_.reset();
        transport_.reset();
        connected_ = false;
        log("Disconnected. Instrument returned to local control.");
    });
}

void App::stopAll() {
    if (sweeping_) {
        runner_.cancel();
        log("Stopping sweep ...");
    }
    monitoring_ = false;
}

void App::startSweep(SweepKind kind, int t) {
    if (!connected_ || worker_->busy()) return;
    SweepConfig c = cfg_[int(kind)];
    c.trigger = trigger_;
    {
        std::lock_guard<std::mutex> lk(data_);
        auto& tr0 = traces_[int(kind)][0];
        if (kind == SweepKind::FrequencyResponse && t > 0 && frReuseRef_ &&
            tr0.result.refLevelV > 0)
            c.refLevelV = tr0.result.refLevelV;
        auto& tr = traces_[int(kind)][t];
        tr.result = SweepResult{};
        tr.result.config = c;
        tr.visible = true;
    }
    sweeping_ = true;
    sweepKind_ = int(kind);
    sweepTrace_ = t;
    progress_ = 0;
    progressTotal_ = c.points;
    fitPlot_ = true;
    log(std::string("Sweep ") + name(kind) + " -> " + traces_[int(kind)][t].name);

    worker_->post([this, kind, t, c] {
        const double tmo = readTimeout_;
        analyzer_->readTimeout = tmo;
        SweepResult res = runner_.run(
            *analyzer_, c,
            [&](const SweepPoint& p, int i, int n) {
                std::lock_guard<std::mutex> lk(data_);
                traces_[int(kind)][t].result.points.push_back(p);
                progress_ = i + 1;
                progressTotal_ = n;
                fitPlot_ = true;
            },
            [&](const std::string& s) { log(s); });
        {
            std::lock_guard<std::mutex> lk(data_);
            auto& tr = traces_[int(kind)][t];
            auto pts = std::move(tr.result.points);
            tr.result = res;
            if (tr.result.points.empty()) tr.result.points = std::move(pts);
        }
        sweeping_ = false;
        sweepKind_ = -1;
        if (!res.error.empty() && !res.cancelled) log("Sweep stopped: " + res.error);
        else if (res.cancelled || !res.error.empty()) log("Sweep cancelled.");
        else {
            char b[96];
            double total = 0;
            for (auto& p : res.points) total += p.elapsed;
            std::snprintf(b, sizeof b, "Sweep complete: %zu points in %.0f s",
                          res.points.size(), total);
            log(b);
        }
    });
}

void App::startMonitor() {
    if (!connected_ || worker_->busy()) return;
    monitoring_ = true;
    {
        std::lock_guard<std::mutex> lk(data_);
        monHistory_.clear();
    }
    ++monVersion_;
    log("Monitor running.");
    worker_->post([this] {
        int seen = -1;
        MonitorCfg c;
        double settle = 0;
        try {
            while (monitoring_) {
                if (monVersion_ != seen) {
                    {
                        std::lock_guard<std::mutex> lk(data_);
                        c = mon_;
                    }
                    seen = monVersion_;
                    analyzer_->setTrigger(trigger_);
                    analyzer_->setLog(false);
                    analyzer_->setFilters(c.lp, c.hp);
                    analyzer_->setFrequency(c.freq);
                    if (c.sourceOn) analyzer_->setAmplitude(c.level);
                    else analyzer_->sourceOff();
                    settle = 0.3;
                    std::lock_guard<std::mutex> lk(data_);
                    monHistory_.clear();
                }
                const Measurement m = kMonitorModes[c.meas].m;
                Reading r;
                try {
                    r = analyzer_->measure(m, settle);
                } catch (const gpib::Timeout& e) {
                    r.raw = e.what();
                }
                settle = 0;
                std::lock_guard<std::mutex> lk(data_);
                monValid_ = r.valid;
                if (r.valid) {
                    monLast_ = r.value;
                    monReading_ = fmtEng(r.value, unit(m, false));
                    monHistory_.push_back(r.value);
                    if (monHistory_.size() > 300) monHistory_.erase(monHistory_.begin());
                } else {
                    monReading_ = r.raw.empty() ? "no reading" : r.describe();
                }
            }
            analyzer_->sourceOff();
            log("Monitor stopped. Source off.");
        } catch (const std::exception& e) {
            monitoring_ = false;
            log(std::string("Monitor stopped: ") + e.what());
        }
    });
}

void App::sendRaw(const std::string& codes, bool read) {
    if (!connected_ || worker_->busy()) return;
    log(">> " + codes);
    worker_->post([this, codes, read] {
        try {
            analyzer_->send(codes);
            if (read) {
                std::string r = analyzer_->readRaw(readTimeout_);
                while (!r.empty() && (r.back() == '\n' || r.back() == '\r')) r.pop_back();
                Reading p = Analyzer::parse(r);
                log("<< " + r + (p.valid ? "   (" + std::to_string(p.value) + ")" : ""));
            }
        } catch (const std::exception& e) {
            log(std::string("Error: ") + e.what());
        }
    });
}

void App::exportCsv(SweepKind kind) {
    std::vector<SweepResult> res;
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lk(data_);
        for (auto& tr : traces_[int(kind)])
            if (tr.visible && !tr.result.points.empty()) {
                res.push_back(tr.result);
                names.push_back(tr.name);
            }
    }
    if (res.empty()) { log("Nothing to export yet."); return; }
    makeDirs(documentsDir());
    static const char* slug[kKinds] = {"freq-response", "thdn", "voltage",
                                       "power-vs-level", "power-vs-freq"};
    std::string p = joinPath(documentsDir(), std::string(slug[int(kind)]) + "_" + nowStamp() + ".csv");
    std::ofstream(p) << toCsv(res, names);
    log("Saved " + p);
    reveal(p);
}

std::string App::screenshotPath() const {
    if (const char* p = std::getenv("HP8903_SHOT")) return p;
    makeDirs(documentsDir());
    return joinPath(documentsDir(), "hp8903_" + nowStamp() + ".png");
}

void App::screenshotSaved(const std::string& path) {
    log("Screenshot saved " + path);
    if (selfTest_.empty()) reveal(path);
}

void App::reveal(const std::string& path) {
#if defined(__APPLE__)
    std::string cmd = "open -R \"" + path + "\" &";
#elif defined(_WIN32)
    std::string cmd = "explorer /select,\"" + path + "\"";
#else
    std::string cmd = "xdg-open \"" + parentDir(path) + "\" >/dev/null 2>&1 &";
#endif
    if (std::system(cmd.c_str()) != 0) {}
}

// ===========================================================================
// UI
// ===========================================================================

void App::frame() {
    if (!selfTest_.empty()) selfTestStep();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("HP 8903", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    drawToolbar();
    ImGui::Separator();

    const float statusH = ImGui::GetFrameHeightWithSpacing() + 4;
    ImGui::BeginChild("body", ImVec2(0, -statusH));
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Frequency response", nullptr, forceTab_ == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
            drawSweepTab(SweepKind::FrequencyResponse);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("THD+N", nullptr, forceTab_ == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
            drawSweepTab(SweepKind::ThdVsFrequency);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Voltage", nullptr, forceTab_ == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
            drawSweepTab(SweepKind::VoltageVsFrequency);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Power", nullptr, forceTab_ == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
            drawSweepTab(powerMode_ == 0 ? SweepKind::PowerVsLevel : SweepKind::PowerVsFrequency);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Monitor", nullptr, forceTab_ == 4 ? ImGuiTabItemFlags_SetSelected : 0)) {
            drawMonitor();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Console")) {
            drawConsole();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    forceTab_ = -1;
    ImGui::EndChild();
    drawStatusBar();
    ImGui::End();

    if (showInstrumentSettings_) drawInstrumentSettings();
}

void App::drawToolbar() {
    const bool busy = worker_->busy();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Adapter");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    ImGui::BeginDisabled(connected_ || busy);
    if (ImGui::BeginCombo("##adapter", adapters_.empty() ? "none" : adapters_[adapterIndex_].name.c_str())) {
        for (size_t i = 0; i < adapters_.size(); ++i)
            if (ImGui::Selectable(adapters_[i].name.c_str(), int(i) == adapterIndex_))
                adapterIndex_ = int(i);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) refreshAdapters();
    ImGui::SameLine();
    ImGui::TextUnformatted("GPIB address");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    if (ImGui::InputInt("##addr", &address_)) address_ = std::clamp(address_, 0, 30);
    ImGui::EndDisabled();
    ImGui::SameLine();

    if (!connected_) {
        ImGui::BeginDisabled(busy || adapters_.empty());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.45f, 0.25f, 1));
        if (ImGui::Button("Connect")) connect();
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Disconnect")) disconnect();
    }
    ImGui::SameLine();
    if (ImGui::Button("Instrument...")) showInstrumentSettings_ = true;

    ImGui::SameLine();
    const float r = ImGui::GetFontSize() * 0.4f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.x += r + 4;
    p.y += ImGui::GetFrameHeight() * 0.5f;
    ImGui::GetWindowDrawList()->AddCircleFilled(
        p, r, connected_ ? IM_COL32(80, 220, 110, 255) : IM_COL32(120, 120, 120, 255));
    ImGui::Dummy(ImVec2(r * 2 + 8, ImGui::GetFrameHeight()));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (connected_) ImGui::TextUnformatted(("HP " + std::string(modelName_) + " on " + connectedTo_).c_str());
    else ImGui::TextDisabled("not connected");
}

void App::drawFilterCombos(LowPass& lp, PlugIn& hp) {
    const char* lps[] = {"Off", "30 kHz LP", "80 kHz LP"};
    int l = int(lp);
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 7.5f);
    if (ImGui::Combo("Low-pass", &l, lps, 3)) lp = LowPass(l);
    ImGui::SetItemTooltip("Low-pass filter (L0 / L1 / L2)");
    const char* hps[] = {"Off", h1Name_, h2Name_};
    int h = int(hp);
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 7.5f);
    if (ImGui::Combo("High-pass", &h, hps, 3)) hp = PlugIn(h);
    ImGui::SetItemTooltip("High-pass / weighting filter (H0 / H1 / H2). Names set under Instrument...");
}

void App::drawSweepSettings(SweepKind kind) {
    auto& c = cfg_[int(kind)];
    const bool running = sweeping_;
    ImGui::BeginDisabled(running);
    ImGui::PushItemWidth(-ImGui::GetFontSize() * 7.5f);

    if (kind == SweepKind::PowerVsLevel || kind == SweepKind::PowerVsFrequency) {
        ImGui::RadioButton("vs level", &powerMode_, 0);
        ImGui::SameLine();
        ImGui::RadioButton("vs frequency", &powerMode_, 1);
    }

    ImGui::SeparatorText("Sweep");
    if (kind != SweepKind::PowerVsLevel) {
        ImGui::InputDouble("Start (Hz)", &c.startHz, 0, 0, "%.0f");
        ImGui::InputDouble("Stop (Hz)", &c.stopHz, 0, 0, "%.0f");
        ImGui::InputDouble("Level (Vrms)", &c.levelV, 0, 0, "%.4f");
    } else {
        ImGui::InputDouble("Frequency (Hz)", &c.fixedHz, 0, 0, "%.0f");
        ImGui::InputDouble("From (Vrms)", &c.startLevelV, 0, 0, "%.4f");
        ImGui::InputDouble("To (Vrms)", &c.stopLevelV, 0, 0, "%.4f");
    }
    ImGui::InputInt("Points", &c.points);
    c.points = std::clamp(c.points, 2, 500);
    c.startHz = std::clamp(c.startHz, 20.0, 100000.0);
    c.stopHz = std::clamp(c.stopHz, 20.0, 100000.0);
    c.levelV = std::clamp(c.levelV, 0.0, 6.0);
    c.startLevelV = std::clamp(c.startLevelV, 0.0, 6.0);
    c.stopLevelV = std::clamp(c.stopLevelV, 0.0, 6.0);

    if (kind == SweepKind::FrequencyResponse) {
        ImGui::InputDouble("Reference (Hz)", &c.refHz, 0, 0, "%.0f");
        ImGui::Checkbox("TR2-4 use TR1 reference", &frReuseRef_);
        helpMarker("Traces 2-4 are normalised to TR1's reference level, so they share "
                   "one dB scale (e.g. left vs right channel).");
    }
    if (kind == SweepKind::PowerVsLevel || kind == SweepKind::PowerVsFrequency)
        ImGui::InputDouble("Load (ohm)", &c.loadOhm, 0, 0, "%.1f");

    ImGui::SeparatorText("Filters");
    drawFilterCombos(c.lowPass, c.plugIn);

    ImGui::SeparatorText("Timing");
    ImGui::InputDouble("Settle (s)", &c.settle, 0, 0, "%.2f");
    ImGui::InputDouble("Below 100 Hz (s)", &c.lowFreqSettle, 0, 0, "%.2f");
    c.settle = std::clamp(c.settle, 0.0, 10.0);
    c.lowFreqSettle = std::clamp(c.lowFreqSettle, 0.0, 10.0);
    ImGui::PopItemWidth();
    ImGui::EndDisabled();

    ImGui::SeparatorText("Traces");
    const bool canRun = connected_ && !worker_->busy();
    for (int t = 0; t < kTraces; ++t) {
        ImGui::PushID(t);
        auto& tr = traces_[int(kind)][t];
        const bool active = running && sweepKind_ == int(kind) && sweepTrace_ == t;
        ImVec4 col = kTraceColors[t];
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(col.x * 0.55f, col.y * 0.55f, col.z * 0.55f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(col.x * 0.75f, col.y * 0.75f, col.z * 0.75f, 1));
        ImGui::BeginDisabled(!canRun);
        char lbl[48];
        std::snprintf(lbl, sizeof lbl, active ? "Running..." : "Run %s", tr.name);
        if (ImGui::Button(lbl, ImVec2(ImGui::GetFontSize() * 7, 0))) startSweep(kind, t);
        ImGui::EndDisabled();
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::Checkbox("##vis", &tr.visible);
        ImGui::SetItemTooltip("Show / hide");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - 6);
        ImGui::InputText("##name", tr.name, sizeof tr.name);
        ImGui::SameLine();
        ImGui::BeginDisabled(active);
        if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) {
            std::lock_guard<std::mutex> lk(data_);
            tr.result = SweepResult{};
        }
        ImGui::SetItemTooltip("Clear this trace");
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    ImGui::Spacing();
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::BeginDisabled(!running);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.16f, 1));
    if (ImGui::Button("Stop", ImVec2(w, 0))) stopAll();
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Fit view", ImVec2(w, 0))) fitPlot_ = true;
    if (ImGui::Button("Export CSV", ImVec2(w, 0))) exportCsv(kind);
    ImGui::SameLine();
    if (ImGui::Button("Screenshot", ImVec2(w, 0))) screenshot_ = true;
    if (kind == SweepKind::ThdVsFrequency || kind == SweepKind::PowerVsLevel ||
        kind == SweepKind::PowerVsFrequency)
        ImGui::Checkbox("Log scale for THD+N", &logY_[int(kind)]);
}

void App::drawSweepTab(SweepKind kind) {
    const float side = ImGui::GetFontSize() * 21;
    ImGui::BeginChild("settings", ImVec2(side, 0), ImGuiChildFlags_Borders);
    drawSweepSettings(kind);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("plot", ImVec2(0, 0));
    drawPlot(kind);
    ImGui::EndChild();
}

void App::drawPlot(SweepKind kind) {
    std::lock_guard<std::mutex> lk(data_);
    const auto& c = cfg_[int(kind)];
    const bool freqX = kind != SweepKind::PowerVsLevel;
    const bool hasY2 = kind == SweepKind::ThdVsFrequency || kind == SweepKind::PowerVsLevel ||
                       kind == SweepKind::PowerVsFrequency;
    const char* y1 = "Level (dB)";
    const char* y2 = nullptr;
    bool thdOnY1 = false;
    switch (kind) {
        case SweepKind::FrequencyResponse: y1 = "Level (dB re ref)"; break;
        case SweepKind::ThdVsFrequency: y1 = "THD+N (%)"; y2 = "Level (Vrms)"; thdOnY1 = true; break;
        case SweepKind::VoltageVsFrequency: y1 = "Level (Vrms)"; break;
        default: y1 = "Power (W)"; y2 = "THD+N (%)"; break;
    }

    if (fitPlot_.exchange(false)) ImPlot::SetNextAxesToFit();
    if (!ImPlot::BeginPlot("##plot", ImVec2(-1, -1), ImPlotFlags_NoTitle)) return;

    ImPlot::SetupAxis(ImAxis_X1, freqX ? "Frequency (Hz)" : "Source level (Vrms)");
    if (freqX) {
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
        ImPlot::SetupAxisLimits(ImAxis_X1, c.startHz, c.stopHz, ImPlotCond_Once);
    }
    ImPlot::SetupAxis(ImAxis_Y1, y1);
    if (kind == SweepKind::FrequencyResponse) ImPlot::SetupAxisLimits(ImAxis_Y1, -6, 2, ImPlotCond_Once);
    if (hasY2) ImPlot::SetupAxis(ImAxis_Y2, y2, ImPlotAxisFlags_AuxDefault);
    if (logY_[int(kind)] && hasY2)
        ImPlot::SetupAxisScale(thdOnY1 ? ImAxis_Y1 : ImAxis_Y2, ImPlotScale_Log10);
    ImPlot::SetupLegend(ImPlotLocation_NorthEast);

    for (int t = 0; t < kTraces; ++t) {
        const auto& tr = traces_[int(kind)][t];
        if (!tr.visible || tr.result.points.empty()) continue;
        const auto& pts = tr.result.points;
        std::vector<double> xs, a, b;
        xs.reserve(pts.size());
        for (auto& p : pts) { xs.push_back(p.x); a.push_back(p.y1); b.push_back(p.y2); }
        ImPlotSpec s;
        s.LineColor = kTraceColors[t];
        s.LineWeight = 2;
        s.Marker = ImPlotMarker_Circle;
        s.MarkerSize = 2.5f;
        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
        ImPlot::PlotLine(tr.name, xs.data(), a.data(), int(xs.size()), s);
        if (hasY2) {
            ImPlotSpec s2;
            s2.LineColor = ImVec4(kTraceColors[t].x, kTraceColors[t].y, kTraceColors[t].z, 0.45f);
            s2.LineWeight = 1;
            s2.Flags = ImPlotItemFlags_NoLegend;
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            char id[48];
            std::snprintf(id, sizeof id, "%s##y2", tr.name);
            ImPlot::PlotLine(id, xs.data(), b.data(), int(xs.size()), s2);
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
        }
    }
    if (kind == SweepKind::FrequencyResponse) {
        const double lines[] = {0.0, -3.0};
        ImPlotSpec s;
        s.LineColor = ImVec4(1, 1, 1, 0.25f);
        s.Flags = ImPlotInfLinesFlags_Horizontal | ImPlotItemFlags_NoLegend;
        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
        ImPlot::PlotInfLines("##ref", lines, 2, s);
    }

    // Nearest-point readout.
    if (ImPlot::IsPlotHovered()) {
        ImPlotPoint m = ImPlot::GetPlotMousePos(ImAxis_X1, ImAxis_Y1);
        bool any = false;
        for (int t = 0; t < kTraces; ++t) {
            const auto& tr = traces_[int(kind)][t];
            if (!tr.visible || tr.result.points.empty()) continue;
            const SweepPoint* best = nullptr;
            double bd = 1e300;
            for (auto& p : tr.result.points) {
                double d = freqX ? std::fabs(std::log10(p.x) - std::log10(std::max(m.x, 1e-9)))
                                 : std::fabs(p.x - m.x);
                if (d < bd) { bd = d; best = &p; }
            }
            if (!best) continue;
            if (!any) { ImGui::BeginTooltip(); any = true; }
            ImGui::TextColored(kTraceColors[t], "%s", tr.name);
            ImGui::SameLine();
            if (freqX) ImGui::Text("%.0f Hz:", best->x);
            else ImGui::Text("%.4f V:", best->x);
            ImGui::SameLine();
            ImGui::Text("%.5g %s", best->y1, y1);
            if (hasY2) {
                ImGui::SameLine();
                ImGui::TextDisabled("| %.5g %s", best->y2, y2);
            }
        }
        if (any) ImGui::EndTooltip();
    }
    ImPlot::EndPlot();
}

void App::drawMonitor() {
    const bool running = monitoring_;
    const float side = ImGui::GetFontSize() * 21;
    ImGui::BeginChild("monset", ImVec2(side, 0), ImGuiChildFlags_Borders);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(data_);
        ImGui::PushItemWidth(-ImGui::GetFontSize() * 7.5f);
        ImGui::SeparatorText("Measurement");
        const char* labels[std::size(kMonitorModes)];
        for (size_t i = 0; i < std::size(kMonitorModes); ++i) labels[i] = kMonitorModes[i].label;
        changed |= ImGui::Combo("Mode", &mon_.meas, labels, int(std::size(kMonitorModes)));
        ImGui::SeparatorText("Source");
        changed |= ImGui::Checkbox("Source on", &mon_.sourceOn);
        changed |= ImGui::InputDouble("Frequency (Hz)", &mon_.freq, 0, 0, "%.0f", ImGuiInputTextFlags_EnterReturnsTrue);
        changed |= ImGui::InputDouble("Level (Vrms)", &mon_.level, 0, 0, "%.4f", ImGuiInputTextFlags_EnterReturnsTrue);
        mon_.freq = std::clamp(mon_.freq, 20.0, 100000.0);
        mon_.level = std::clamp(mon_.level, 0.0, 6.0);
        ImGui::PopItemWidth();
        ImGui::SeparatorText("Filters");
        LowPass lp = mon_.lp; PlugIn hp = mon_.hp;
        drawFilterCombos(lp, hp);
        if (lp != mon_.lp || hp != mon_.hp) { mon_.lp = lp; mon_.hp = hp; changed = true; }
    }
    if (changed) ++monVersion_;
    ImGui::TextDisabled("Press Enter to apply a typed value.");
    ImGui::Spacing();
    const float w = ImGui::GetContentRegionAvail().x;
    if (!running) {
        ImGui::BeginDisabled(!connected_ || worker_->busy());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.45f, 0.25f, 1));
        if (ImGui::Button("Start monitor", ImVec2(w, 0))) startMonitor();
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.16f, 1));
        if (ImGui::Button("Stop monitor", ImVec2(w, 0))) monitoring_ = false;
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("monview", ImVec2(0, 0));
    std::lock_guard<std::mutex> lk(data_);
    ImGui::TextDisabled("%s", kMonitorModes[mon_.meas].label);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 4.0f);
    ImGui::TextColored(monValid_ ? ImVec4(0.55f, 0.95f, 0.60f, 1) : ImVec4(0.9f, 0.6f, 0.4f, 1),
                       "%s", running || !monHistory_.empty() ? monReading_.c_str() : "-");
    ImGui::PopFont();
    if (monHistory_.size() > 1) {
        double mn = *std::min_element(monHistory_.begin(), monHistory_.end());
        double mx = *std::max_element(monHistory_.begin(), monHistory_.end());
        ImGui::Text("min %.6g   max %.6g   span %.3g%%", mn, mx,
                    mx != 0 ? 100.0 * (mx - mn) / std::fabs(mx) : 0.0);
    }
    if (ImPlot::BeginPlot("##hist", ImVec2(-1, -1), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("readings", kMonitorModes[mon_.meas].label,
                          ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        if (!monHistory_.empty()) {
            ImPlotSpec s;
            s.LineColor = ImVec4(0.55f, 0.95f, 0.60f, 1);
            s.LineWeight = 1.5f;
            ImPlot::PlotLine("history", monHistory_.data(), int(monHistory_.size()), 1.0, 0.0, s);
        }
        ImPlot::EndPlot();
    }
    ImGui::EndChild();
}

void App::drawConsole() {
    ImGui::TextDisabled("Send raw HP 8903 program codes, e.g. FR1000HZ AP1VL M1, 400 Hz filter H1, "
                        "weighting H2, LG/LN log/linear.");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
    bool enter = ImGui::InputText("##cmd", consoleInput_, sizeof consoleInput_,
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(!connected_ || worker_->busy());
    bool send = ImGui::Button("Send");
    ImGui::SameLine();
    bool sendRead = ImGui::Button("Send + read") || enter;
    ImGui::SameLine();
    bool readOnly = ImGui::Button("Read");
    ImGui::EndDisabled();
    if ((send || sendRead) && consoleInput_[0]) sendRaw(consoleInput_, sendRead);
    if (readOnly && connected_) {
        worker_->post([this] {
            try {
                std::string r = analyzer_->readRaw(readTimeout_);
                while (!r.empty() && (r.back() == '\n' || r.back() == '\r')) r.pop_back();
                log("<< " + r);
            } catch (const std::exception& e) { log(std::string("Error: ") + e.what()); }
        });
    }
    if (enter) ImGui::SetKeyboardFocusHere(-1);

    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders);
    std::lock_guard<std::mutex> lk(data_);
    for (auto& l : log_) ImGui::TextUnformatted(l.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void App::drawStatusBar() {
    ImGui::Separator();
    std::string st;
    {
        std::lock_guard<std::mutex> lk(data_);
        st = status_;
    }
    if (sweeping_) {
        int n = progressTotal_, i = progress_;
        char b[32];
        std::snprintf(b, sizeof b, "%d / %d", i, n);
        ImGui::ProgressBar(n ? float(i) / n : 0, ImVec2(ImGui::GetFontSize() * 14, 0), b);
        ImGui::SameLine();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(st.c_str());
}

void App::drawInstrumentSettings() {
    ImGui::OpenPopup("Instrument");
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30, 0));
    if (ImGui::BeginPopupModal("Instrument", &showInstrumentSettings_)) {
        ImGui::PushItemWidth(-ImGui::GetFontSize() * 11);
        ImGui::InputText("Model", modelName_, sizeof modelName_);
        ImGui::SeparatorText("High-pass / weighting filters");
        ImGui::InputText("H1 filter name", h1Name_, sizeof h1Name_);
        ImGui::InputText("H2 filter name", h2Name_, sizeof h2Name_);
        ImGui::TextWrapped("8903A: H1 is the built-in 400 Hz high-pass, H2 the optional "
                           "weighting filter. 8903B: H1/H2 are the left/right plug-in slots.");
        ImGui::SeparatorText("Timing");
        ImGui::InputDouble("Read timeout (s)", &readTimeout_, 0, 0, "%.0f");
        readTimeout_ = std::clamp(readTimeout_, 2.0, 120.0);
        bool settled = trigger_ == Trigger::Settled;
        if (ImGui::Checkbox("Settled trigger (T3)", &settled))
            trigger_ = settled ? Trigger::Settled : Trigger::FreeRun;
        helpMarker("Experimental: ask the 8903 to trigger a measurement and return it only once "
                   "settled, instead of reading the free-running display after a fixed delay. "
                   "Can be faster and more reliable at low frequencies. Untested on hardware.");
        ImGui::PopItemWidth();
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(ImGui::GetFontSize() * 6, 0))) {
            showInstrumentSettings_ = false;
            saveSettings();
        }
        ImGui::EndPopup();
    }
    if (!showInstrumentSettings_) saveSettings();
}
