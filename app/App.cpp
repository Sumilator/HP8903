// SPDX-License-Identifier: GPL-2.0-only
#include "App.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cfloat>
#include <cstdint>
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

// ---------------------------------------------------------------------------
// Derived values shown under the main readings.

std::vector<std::string> derivedLines(Measurement m, bool valid, double x,
                                      double sourceV, double loadOhm) {
    std::vector<std::string> out;
    if (!valid) return out;
    char b[160];
    switch (m) {
        case Measurement::AcLevel:
        case Measurement::DistortionLevel:
            if (x > 0) {
                std::snprintf(b, sizeof b, "%+.2f dBV   %+.2f dBu", 20 * std::log10(x),
                              20 * std::log10(x / 0.774597));
                out.push_back(b);
            }
            if (m == Measurement::AcLevel && loadOhm > 0) {
                const double w = x * x / loadOhm;
                if (w < 1) std::snprintf(b, sizeof b, "%.3f mW into %g ohm", w * 1e3, loadOhm);
                else std::snprintf(b, sizeof b, "%.3f W into %g ohm", w, loadOhm);
                out.push_back(b);
            }
            if (m == Measurement::AcLevel && sourceV > 0 && x > 0) {
                std::snprintf(b, sizeof b, "Gain %+.2f dB (x%.3f) re source", 20 * std::log10(x / sourceV),
                              x / sourceV);
                out.push_back(b);
            }
            break;
        case Measurement::Distortion:
            if (x > 0) {
                const double db = 20 * std::log10(x / 100);
                std::snprintf(b, sizeof b, "%.1f dB   =   SINAD %.1f dB   (%.0f ppm)", db, -db, x * 1e4);
                out.push_back(b);
            }
            break;
        case Measurement::Sinad:
            std::snprintf(b, sizeof b, "=   THD+N %.4f %%", 100 * std::pow(10.0, -x / 20));
            out.push_back(b);
            break;
        case Measurement::SignalToNoise:
            std::snprintf(b, sizeof b, "noise %.4f %% of signal", 100 * std::pow(10.0, -x / 20));
            out.push_back(b);
            break;
        case Measurement::DcLevel:
            break;
    }
    return out;
}

// The 8903 shows 4 significant digits; mirror that.
std::string displayValue(Measurement m, double v, std::string* unitOut) {
    char b[48];
    const double a = std::fabs(v);
    switch (m) {
        case Measurement::AcLevel:
        case Measurement::DistortionLevel:
        case Measurement::DcLevel:
            if (a < 1e-3) { std::snprintf(b, sizeof b, "%.2f", v * 1e6); *unitOut = "uV"; }
            else if (a < 1) { std::snprintf(b, sizeof b, a < 0.01 ? "%.3f" : a < 0.1 ? "%.2f" : "%.1f", v * 1e3); *unitOut = "mV"; }
            else { std::snprintf(b, sizeof b, a < 10 ? "%.4f" : "%.3f", v); *unitOut = "V"; }
            break;
        case Measurement::Distortion:
            std::snprintf(b, sizeof b, a < 0.01 ? "%.5f" : a < 0.1 ? "%.4f" : a < 1 ? "%.3f" : "%.2f", v);
            *unitOut = "%";
            break;
        default:
            std::snprintf(b, sizeof b, "%.2f", v);
            *unitOut = "dB";
            break;
    }
    return b;
}

std::string fmtHz(double hz) {
    char b[32];
    if (hz >= 10000) std::snprintf(b, sizeof b, "%.2f kHz", hz / 1000);
    else if (hz >= 1000) std::snprintf(b, sizeof b, "%.3f kHz", hz / 1000);
    else std::snprintf(b, sizeof b, "%.1f Hz", hz);
    return b;
}


// ---------------------------------------------------------------------------
// Seven-segment LED display modelled on the HP 8903A front panel: four
// digits per display, each in its own dark cell behind the red filter, thin
// slanted segments with clear gaps, unlit segments hidden by the filter, and
// small red annunciator LEDs for the units.
//
//      aaa
//     f   b
//      ggg
//     e   c
//      ddd  .
uint8_t segMask(char c) {
    switch (c) {
        case '0': return 0x3F; case '1': return 0x06; case '2': return 0x5B;
        case '3': return 0x4F; case '4': return 0x66; case '5': return 0x6D;
        case '6': return 0x7D; case '7': return 0x07; case '8': return 0x7F;
        case '9': return 0x6F; case '-': return 0x40; case 'E': return 0x79;
        case 'r': return 0x50; case 'o': return 0x5C; case 'H': return 0x76;
        case 'L': return 0x38; case 'P': return 0x73; case 'F': return 0x71;
        default: return 0x00;
    }
}

struct Led {
    float h;                                     // digit height in pixels
    // Proportions measured from an 8903A front panel.
    float w() const { return h * 0.62f; }        // digit width (unslanted)
    float t() const { return h * 0.09f; }        // segment thickness
    float gap() const { return h * 0.035f; }     // gap between segments
    float slant() const { return 0.14f; }        // about 8 degrees
    float cellH() const { return h * 1.70f; }    // dark cell behind a digit
    float cellW() const { return h * 1.15f; }
    float pitch() const { return h * 1.20f; }
};

const ImU32 kLedOn = IM_COL32(255, 84, 42, 255);
const ImU32 kLedUnlit = IM_COL32(47, 10, 8, 255);   // barely visible, as behind the filter
const ImU32 kLedCell = IM_COL32(42, 9, 7, 255);
const ImU32 kLedWindow = IM_COL32(20, 4, 3, 255);

// Rectangular segment, sheared to the display's slant.
void ledSegment(ImDrawList* dl, const Led& L, ImVec2 o, int seg, ImU32 col) {
    const float w = L.w(), h = L.h, t = L.t(), g = L.gap(), m = h * 0.5f;
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    switch (seg) {
        case 0: x0 = t + g; x1 = w - t - g; y0 = 0;           y1 = t;           break;  // a
        case 1: x0 = w - t; x1 = w;         y0 = g;           y1 = m - g * 0.5f; break; // b
        case 2: x0 = w - t; x1 = w;         y0 = m + g * 0.5f; y1 = h - g;       break;  // c
        case 3: x0 = t + g; x1 = w - t - g; y0 = h - t;       y1 = h;           break;  // d
        case 4: x0 = 0;     x1 = t;         y0 = m + g * 0.5f; y1 = h - g;       break;  // e
        case 5: x0 = 0;     x1 = t;         y0 = g;           y1 = m - g * 0.5f; break; // f
        case 6: x0 = t + g; x1 = w - t - g; y0 = m - t * 0.5f; y1 = m + t * 0.5f; break; // g
    }
    auto P = [&](float x, float y) { return ImVec2(o.x + x + (h - y) * L.slant(), o.y + y); };
    const ImVec2 q[4] = {P(x0, y0), P(x1, y0), P(x1, y1), P(x0, y1)};
    dl->AddConvexPolyFilled(q, 4, col);
}

// Draws `cells` digit cells from `o` (top-left of the first cell). `s` may
// start with '-'. The 8903A's decimal points sit at the bottom LEFT of a
// digit, so a '.' belongs to the digit that follows it (".0182").
// Digits are right-aligned in the cells, like the instrument.
void ledDigits(ImDrawList* dl, const Led& L, ImVec2 o, const std::string& s, int cells) {
    struct Cell { char c = ' '; bool dp = false; };
    bool neg = false, pendingDp = false;
    std::vector<Cell> v;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '-' && i == 0 && s.size() > 1) { neg = true; continue; }
        if (c == '.') { pendingDp = true; continue; }
        v.push_back(Cell{c, pendingDp});
        pendingDp = false;
    }
    while (int(v.size()) < cells) v.insert(v.begin(), Cell{});
    while (int(v.size()) > cells) v.erase(v.begin());

    const float digitSpan = L.w() + L.h * L.slant();
    for (int i = 0; i < cells; ++i) {
        const ImVec2 c0(o.x + i * L.pitch(), o.y);
        const ImVec2 c1(c0.x + L.cellW(), c0.y + L.cellH());
        dl->AddRectFilled(c0, c1, kLedCell, 1.5f);
        const ImVec2 d(c0.x + (L.cellW() - digitSpan) * 0.5f + L.h * 0.06f,
                       c0.y + (L.cellH() - L.h) * 0.5f);
        const uint8_t m = segMask(v[i].c);
        for (int sgi = 0; sgi < 7; ++sgi)
            ledSegment(dl, L, d, sgi, (m >> sgi) & 1 ? kLedOn : kLedUnlit);
        // round decimal point, low and to the left of the digit
        dl->AddCircleFilled(ImVec2(d.x - L.h * 0.13f, d.y + L.h * 0.97f), L.h * 0.065f,
                            v[i].dp ? kLedOn : kLedUnlit, 16);
    }
    // Minus sign: a short bar left of the first cell.
    const float my = o.y + L.cellH() * 0.5f, mx = o.x - L.h * 0.12f;
    dl->AddRectFilled(ImVec2(mx - L.h * 0.22f, my - L.t() * 0.5f), ImVec2(mx, my + L.t() * 0.5f),
                      neg ? kLedOn : kLedUnlit);
}

float ledSignW(const Led& L) { return L.h * 0.40f; }
float ledDigitsWidth(const Led& L, int cells) {
    return ledSignW(L) + (cells - 1) * L.pitch() + L.cellW();
}

// Four significant digits, the way the 8903 shows them: no leading zero
// when the value is below 1 and all four cells are needed (".0182").
std::string fmt4(double v) {
    const double a = std::fabs(v);
    char b[32];
    if (a == 0) { std::snprintf(b, sizeof b, "0.000"); return b; }
    int mag = int(std::floor(std::log10(a)));
    int dec = std::clamp(3 - mag, 0, 4);
    std::snprintf(b, sizeof b, "%.*f", dec, a);
    auto digits = [](const char* s) { int n = 0; for (; *s; ++s) n += (*s >= '0' && *s <= '9'); return n; };
    std::string s = b;
    if (s.rfind("0.", 0) == 0) {
        if (digits(b) > 4) s.erase(0, 1);                       // "0.0049" -> ".0049"
    } else if (digits(b) > 4 && dec > 0) {                      // rounding carry, 9.9996 -> 10.00
        std::snprintf(b, sizeof b, "%.*f", dec - 1, a);
        s = b;
    }
    return (v < 0 ? "-" : "") + s;
}

// LED digits + which annunciator to light, per measurement.
std::string ledValue(Measurement m, double v, std::string* unit) {
    switch (m) {
        case Measurement::AcLevel:
        case Measurement::DistortionLevel:
        case Measurement::DcLevel:
            if (std::fabs(v) < 1) { *unit = "mV"; return fmt4(v * 1e3); }
            *unit = "V";
            return fmt4(v);
        case Measurement::Distortion:
            *unit = "%";
            return fmt4(v);
        default:
            *unit = "dB";
            return fmt4(v);
    }
}

std::string hzDigits(double hz, std::string* unit) {
    if (hz >= 1000) { *unit = "kHz"; return fmt4(hz / 1000); }
    *unit = "Hz";
    return fmt4(hz);
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
    if (selfTest_ == "panel") forceTab_ = 4;
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
                if (selfTest_ == "monitor" || selfTest_ == "panel") {
                    mon_.meas = 1;
                    mon_.meas2 = 0;
                    mon_.level = 1.2;
                    mon_.hp = PlugIn::Right;
                    mon_.lp = LowPass::LP30k;
                    if (selfTest_ == "panel") bigDisplay_ = true;
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
    mon_.freq = settings_.num("mon.freq", mon_.freq);
    mon_.level = settings_.num("mon.level", mon_.level);
    mon_.meas = std::clamp(int(settings_.num("mon.meas", 0)), 0, int(std::size(kMonitorModes)) - 1);
    mon_.meas2 = std::clamp(int(settings_.num("mon.meas2", -1)), -1, int(std::size(kMonitorModes)) - 1);
    mon_.loadOhm = settings_.num("mon.load", mon_.loadOhm);
    mon_.readCounter = settings_.num("mon.counter", 0) > 0;
    mon_.lp = LowPass(int(settings_.num("mon.lp", 0)));
    mon_.hp = PlugIn(int(settings_.num("mon.hp", 0)));
    ms_.cfg = mon_;
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
    settings_.set("mon.freq", mon_.freq);
    settings_.set("mon.level", mon_.level);
    settings_.set("mon.meas", double(mon_.meas));
    settings_.set("mon.meas2", double(mon_.meas2));
    settings_.set("mon.load", mon_.loadOhm);
    settings_.set("mon.counter", mon_.readCounter ? 1.0 : 0.0);
    settings_.set("mon.lp", double(int(mon_.lp)));
    settings_.set("mon.hp", double(int(mon_.hp)));
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

void App::resetStats() {
    // Caller holds data_.
    ms_.n = 0;
    ms_.sum = ms_.mn = ms_.mx = 0;
    monHistory_.clear();
}

void App::startMonitor() {
    if (!connected_ || worker_->busy()) return;
    monitoring_ = true;
    {
        std::lock_guard<std::mutex> lk(data_);
        resetStats();
        ms_.v1 = ms_.v2 = MonValue{};
        ms_.counterHz = NAN;
        ms_.counterFailed = false;
    }
    ++monVersion_;
    log("Monitor running.");
    worker_->post([this] {
        int seen = -1;
        MonitorCfg c;
        double settle = 0;
        using clock = std::chrono::steady_clock;
        auto readOne = [&](Measurement m, double st) {
            MonValue v;
            v.has = true;
            try {
                Reading r = analyzer_->measure(m, st);
                v.valid = r.valid;
                v.value = r.value;
                v.raw = r.raw;
            } catch (const gpib::Timeout& e) {
                v.raw = e.what();
            }
            return v;
        };
        try {
            while (monitoring_) {
                if (monVersion_ != seen) {
                    {
                        std::lock_guard<std::mutex> lk(data_);
                        c = mon_;
                        resetStats();
                        ms_.v2 = MonValue{};
                        ms_.cfg = c;
                        if (!c.readCounter) ms_.counterHz = NAN;
                    }
                    seen = monVersion_;
                    analyzer_->setTrigger(trigger_);
                    analyzer_->setLog(false);
                    analyzer_->setFilters(c.lp, c.hp);
                    analyzer_->setFrequency(c.freq);
                    if (c.sourceOn) analyzer_->setAmplitude(c.level);
                    else analyzer_->sourceOff();
                    settle = 0.3;
                }
                auto t0 = clock::now();
                MonValue v1 = readOne(kMonitorModes[c.meas].m, settle);
                MonValue v2;
                if (c.meas2 >= 0 && c.meas2 != c.meas)
                    v2 = readOne(kMonitorModes[c.meas2].m, 0.05);
                settle = 0;

                // Left display (input frequency counter): RL selects it for
                // output, RR switches back. HP documents this for the 8903B;
                // if the 8903A doesn't answer sensibly we stop asking.
                double hz = NAN;
                bool counterFailed = false;
                if (c.readCounter) {
                    try {
                        analyzer_->send("RL");
                        Reading r = Analyzer::parse(analyzer_->readRaw(5.0));
                        analyzer_->send("RR");
                        if (r.valid && r.value >= 10 && r.value <= 150000) hz = r.value;
                        else counterFailed = true;
                    } catch (const std::exception&) {
                        counterFailed = true;
                        try { analyzer_->send("RR"); } catch (...) {}
                    }
                }
                const double dt = std::chrono::duration<double>(clock::now() - t0).count();

                std::lock_guard<std::mutex> lk(data_);
                ms_.v1 = v1;
                if (v2.has) ms_.v2 = v2;
                ms_.rate = dt > 0 ? 1.0 / dt : 0;
                if (c.readCounter) {
                    if (counterFailed) {
                        ms_.counterFailed = true;
                        mon_.readCounter = c.readCounter = false;
                        status_ = "This 8903 did not answer the frequency-counter read (RL); "
                                  "showing the source frequency instead.";
                    } else {
                        ms_.counterHz = hz;
                    }
                }
                if (v1.valid) {
                    if (ms_.n == 0) ms_.mn = ms_.mx = v1.value;
                    ms_.mn = std::min(ms_.mn, v1.value);
                    ms_.mx = std::max(ms_.mx, v1.value);
                    ms_.sum += v1.value;
                    ++ms_.n;
                    monHistory_.push_back(v1.value);
                    if (monHistory_.size() > 300) monHistory_.erase(monHistory_.begin());
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
    if (bigDisplay_) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
        ImGui::Begin("HP 8903 display", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
        drawCrt(ImGui::GetContentRegionAvail(), true);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) bigDisplay_ = false;
        if (ImGui::IsKeyPressed(ImGuiKey_F)) fullscreenToggle_ = true;
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return;
    }
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

void App::drawCrt(const ImVec2& size, bool big) {
    MonState s;
    {
        std::lock_guard<std::mutex> lk(data_);
        s = ms_;
        if (!monitoring_ && !s.v1.has) s.cfg = mon_;
    }
    const std::string h1 = h1Name_, h2 = h2Name_;
    const bool running = monitoring_;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    ImFont* f = labelFont_ ? labelFont_ : ImGui::GetFont();

    const ImU32 green = IM_COL32(80, 235, 120, 255);
    const ImU32 greenDim = IM_COL32(52, 165, 88, 255);
    const ImU32 amber = IM_COL32(255, 176, 64, 255);

    // Charcoal display band, as on the instrument.
    dl->AddRectFilled(p0, p1, IM_COL32(36, 36, 38, 255), 8.0f);
    dl->AddRect(p0, p1, IM_COL32(60, 60, 64, 255), 8.0f, 0, 1.5f);

    const float W = size.x, H = size.y;
    const float pad = std::max(10.0f, std::min(W, H) * 0.035f);
    const float innerW = W - 2 * pad;
    auto width = [&](float sz, const std::string& t) {
        return f->CalcTextSizeA(sz, FLT_MAX, 0, t.c_str()).x;
    };
    auto fit = [&](float want, const std::string& t, float maxW) {
        float w = width(want, t);
        return w > maxW ? want * maxW / w : want;
    };
    auto text = [&](bool draw, float sz, float x, float y, ImU32 col, const std::string& t) {
        if (draw && !t.empty()) dl->AddText(f, sz, ImVec2(x, y), col, t.c_str());
    };

    const float small = std::clamp(H * 0.04f, 12.0f, 34.0f);
    const char* lps[] = {"LP off", "30 kHz LP", "80 kHz LP"};
    const std::string hpName = s.cfg.hp == PlugIn::Off ? "HP off" : s.cfg.hp == PlugIn::Left ? h1 : h2;

    // ---------------- header
    float y = p0.y + pad;
    {
        std::string left = "HP " + std::string(modelName_) + " AUDIO ANALYZER";
        if (!connected_) left += "   NOT CONNECTED";
        text(true, small, p0.x + pad, y, greenDim, left);
        char r[48];
        if (running) std::snprintf(r, sizeof r, "RUN  %.1f rdg/s", s.rate);
        else std::snprintf(r, sizeof r, "HOLD");
        text(true, small, p1.x - pad - width(small, r), y, running ? green : amber, r);
        if (big) {
            const std::string hint = "Esc back   F full screen";
            text(true, small * 0.8f, p1.x - pad - width(small * 0.8f, hint) - width(small, r) - small * 2,
                 y + small * 0.1f, greenDim, hint);
        }
        y += small * 1.6f;
    }

    // ---------------- footer, bottom-up
    float footerTop = p1.y - pad;
    {
        char b[220];
        if (s.cfg.sourceOn)
            std::snprintf(b, sizeof b, "SOURCE %.4f V  %s   |  %s  |  %s", s.cfg.level,
                          fmtHz(s.cfg.freq).c_str(), lps[int(s.cfg.lp)], hpName.c_str());
        else
            std::snprintf(b, sizeof b, "SOURCE OFF   |  %s  |  %s", lps[int(s.cfg.lp)], hpName.c_str());
        float sz = fit(small, b, innerW);
        footerTop -= sz;
        text(true, sz, p0.x + pad, footerTop, greenDim, b);
        if (s.n > 1) {
            const Measurement m1 = kMonitorModes[s.cfg.meas].m;
            std::string u;
            std::string mn = displayValue(m1, s.mn, &u), mx = displayValue(m1, s.mx, &u),
                        av = displayValue(m1, s.sum / s.n, &u);
            std::snprintf(b, sizeof b, "MIN %s   MAX %s   AVG %s %s   (%d)", mn.c_str(), mx.c_str(),
                          av.c_str(), u.c_str(), s.n);
            sz = fit(small, b, innerW);
            footerTop -= sz * 1.35f;
            text(true, sz, p0.x + pad, footerTop, green, b);
        }
        footerTop -= small * 0.5f;
    }

    // One display: black-red window, four digit cells, annunciator column(s).
    // `annun` lists the unit labels in rows of `cols`; `lit` is the lit one.
    auto display = [&](bool draw, float x, float yy, float digitH, const char* label,
                       const std::string& digits, const std::vector<std::string>& annun, int cols,
                       const std::string& lit, bool error, int cells = 4) -> ImVec2 {
        Led L{digitH};
        const float lab = small;
        text(draw, lab, x, yy, greenDim, label);
        yy += lab * 1.3f;
        const float padIn = digitH * 0.16f;
        const float annSz = std::max(small * 0.9f, digitH * 0.20f);
        float colW = 0;
        for (auto& a : annun) colW = std::max(colW, width(annSz, a));
        const float dot = annSz * 0.32f;
        const float annW = cols * (dot * 2 + annSz * 0.35f + colW) + (cols - 1) * annSz * 0.8f;
        const float digitsW = ledDigitsWidth(L, cells);
        const float w = padIn + digitsW + digitH * 0.35f + annW + padIn * 2;
        const float h = L.cellH() + padIn * 2;
        if (draw) {
            dl->AddRectFilled(ImVec2(x, yy), ImVec2(x + w, yy + h), kLedWindow, 3.0f);
            ledDigits(dl, L, ImVec2(x + padIn + ledSignW(L), yy + padIn), error ? "Err" : digits, cells);
            const float ax0 = x + padIn + digitsW + digitH * 0.35f;
            // annunciators: red LED dot + label
            const int rows = int((annun.size() + cols - 1) / cols);
            const float rowH = h / (rows + 1);
            for (size_t i = 0; i < annun.size(); ++i) {
                const int c = int(i) % cols, r = int(i) / cols;
                const float ax = ax0 + c * (dot * 2 + annSz * 0.35f + colW + annSz * 0.8f);
                const float ay = yy + rowH * (r + 1);
                const bool on = annun[i] == lit && !error;
                dl->AddCircleFilled(ImVec2(ax + dot, ay), dot, on ? kLedOn : IM_COL32(60, 14, 12, 255));
                text(true, annSz, ax + dot * 2 + annSz * 0.35f, ay - annSz * 0.55f, on ? green : greenDim,
                     annun[i]);
            }
        }
        return ImVec2(w, lab * 1.3f + h);
    };
    auto valueDigits = [&](Measurement m, const MonValue& v, std::string* unit, bool* err) {
        *err = false;
        if (v.valid) return ledValue(m, v.value, unit);
        if (v.has) { *err = true; unit->clear(); return std::string("Err"); }
        unit->clear();
        return std::string("");
    };
    const std::vector<std::string> leftAnn = {"kHz", "Hz"};
    const std::vector<std::string> rightAnn = {"dB", "V", "%", "mV"};

    // ---------------- body, measured then drawn scaled to fit
    auto body = [&](float k, bool draw) -> float {
        float yy = y;
        const float x = p0.x + pad;
        const float gap = pad * 1.2f;

        // Row 1: left display (frequency) and right display (measurement).
        // Size the digits so both windows fit side by side.
        float dh = H * 0.14f * k;
        for (int it = 0; it < 6; ++it) {   // label sizes have a floor, so iterate
            const float wl = display(false, 0, 0, dh, "", "", leftAnn, 1, "", false, 5).x;
            const float wr = display(false, 0, 0, dh, "", "", rightAnn, 2, "", false).x;
            if (wl + gap + wr <= innerW) break;
            dh *= 0.98f * innerW / (wl + gap + wr);
        }
        const bool counted = !std::isnan(s.counterHz);
        std::string fu;
        const std::string fd = hzDigits(counted ? s.counterHz : s.cfg.freq, &fu);
        const ImVec2 lsz = display(draw, x, yy, dh, counted ? "INPUT FREQUENCY" : "SOURCE FREQUENCY",
                                   fd, leftAnn, 1, fu, false, 5);
        const Measurement m1 = kMonitorModes[s.cfg.meas].m;
        std::string u1;
        bool e1;
        const std::string d1 = valueDigits(m1, s.v1, &u1, &e1);
        const float rx = x + lsz.x + gap;
        const ImVec2 rsz = display(draw, rx, yy, dh, kMonitorModes[s.cfg.meas].label, d1, rightAnn, 2,
                                   u1, e1);
        yy += std::max(lsz.y, rsz.y) + small * 0.6f;

        // Derived figures for the main reading, under the right display.
        float dy = yy;
        const float colW = std::max(rsz.x, innerW - (rx - x));
        if (e1 && !s.v1.raw.empty()) {
            const std::string t = "instrument returned " + s.v1.raw;
            const float dsz = fit(small * 1.1f * std::max(k, 0.6f), t, colW);
            text(draw, dsz, rx, dy, amber, t);
            dy += dsz * 1.3f;
        }
        for (auto& line : derivedLines(m1, s.v1.valid, s.v1.value, s.cfg.sourceOn ? s.cfg.level : 0,
                                       s.cfg.loadOhm)) {
            const float dsz = fit(small * 1.15f * std::max(k, 0.6f), line, colW);
            text(draw, dsz, rx, dy, green, line);
            dy += dsz * 1.3f;
        }

        // Row 2: second reading, smaller, under the left display.
        float ly = yy;
        if (s.cfg.meas2 >= 0 && s.cfg.meas2 != s.cfg.meas) {
            const Measurement m2 = kMonitorModes[s.cfg.meas2].m;
            std::string u2;
            bool e2;
            const std::string d2 = valueDigits(m2, s.v2, &u2, &e2);
            float dh2 = dh * 0.62f;
            for (int it = 0; it < 6; ++it) {
                const float w2 = display(false, 0, 0, dh2, "", "", rightAnn, 2, "", false).x;
                if (w2 <= lsz.x) break;
                dh2 *= 0.98f * lsz.x / w2;
            }
            ly += display(draw, x, ly, dh2, kMonitorModes[s.cfg.meas2].label, d2, rightAnn, 2, u2, e2).y;
            ly += small * 0.4f;
            for (auto& line : derivedLines(m2, s.v2.valid, s.v2.value,
                                           s.cfg.sourceOn ? s.cfg.level : 0, s.cfg.loadOhm)) {
                const float dsz = fit(small * 1.05f * std::max(k, 0.6f), line, lsz.x);
                text(draw, dsz, x, ly, green, line);
                ly += dsz * 1.3f;
            }
        }
        return std::max(dy, ly) - y;
    };
    const float need = body(1.0f, false);
    const float room = footerTop - y;
    body(need > room && need > 0 ? room / need : 1.0f, true);

    ImGui::InvisibleButton("##panel", size);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        bigDisplay_ = !bigDisplay_;
    if (!big) ImGui::SetItemTooltip("Double-click for the big display");
}

void App::drawMonitor() {
    const bool running = monitoring_;
    const float side = ImGui::GetFontSize() * 21;
    ImGui::BeginChild("monset", ImVec2(side, 0), ImGuiChildFlags_Borders);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(data_);
        ImGui::PushItemWidth(-ImGui::GetFontSize() * 7.5f);
        ImGui::SeparatorText("Readouts");
        const int nModes = int(std::size(kMonitorModes));
        const char* labels[std::size(kMonitorModes)];
        for (int i = 0; i < nModes; ++i) labels[i] = kMonitorModes[i].label;
        changed |= ImGui::Combo("Main", &mon_.meas, labels, nModes);
        const char* labels2[std::size(kMonitorModes) + 1];
        labels2[0] = "None";
        for (int i = 0; i < nModes; ++i) labels2[i + 1] = kMonitorModes[i].label;
        int m2 = mon_.meas2 + 1;
        if (ImGui::Combo("Second", &m2, labels2, nModes + 1)) { mon_.meas2 = m2 - 1; changed = true; }
        ImGui::SetItemTooltip("Alternate a second measurement with the main one, e.g. THD+N and AC level.");
        changed |= ImGui::InputDouble("Load (ohm)", &mon_.loadOhm, 0, 0, "%.1f", ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetItemTooltip("Used for the power figure derived from AC level.");
        if (ImGui::Checkbox("Read frequency counter", &mon_.readCounter)) changed = true;
        helpMarker("Reads the 8903's left display (measured input frequency) with the RL/RR "
                   "codes. Documented for the 8903B; if your 8903A doesn't answer, the app "
                   "turns this off and shows the source frequency.");
        ImGui::SeparatorText("Source");
        changed |= ImGui::Checkbox("Source on", &mon_.sourceOn);
        changed |= ImGui::InputDouble("Frequency (Hz)", &mon_.freq, 0, 0, "%.0f", ImGuiInputTextFlags_EnterReturnsTrue);
        changed |= ImGui::InputDouble("Level (Vrms)", &mon_.level, 0, 0, "%.4f", ImGuiInputTextFlags_EnterReturnsTrue);
        mon_.freq = std::clamp(mon_.freq, 20.0, 100000.0);
        mon_.level = std::clamp(mon_.level, 0.0, 6.0);
        mon_.loadOhm = std::clamp(mon_.loadOhm, 0.1, 100000.0);
        ImGui::PopItemWidth();
        ImGui::SeparatorText("Filters");
        LowPass lp = mon_.lp; PlugIn hp = mon_.hp;
        drawFilterCombos(lp, hp);
        if (lp != mon_.lp || hp != mon_.hp) { mon_.lp = lp; mon_.hp = hp; changed = true; }
        if (!running) ms_.cfg = mon_;
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
    const float hw = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Big display", ImVec2(hw, 0))) bigDisplay_ = true;
    ImGui::SetItemTooltip("Fill the window with the display (F for full screen, Esc to return)");
    ImGui::SameLine();
    if (ImGui::Button("Reset min/max", ImVec2(hw, 0))) {
        std::lock_guard<std::mutex> lk(data_);
        resetStats();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("monview", ImVec2(0, 0));
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    drawCrt(ImVec2(avail.x, std::max(avail.y * 0.68f, 260.0f)), false);
    std::lock_guard<std::mutex> lk(data_);
    if (ImPlot::BeginPlot("##hist", ImVec2(-1, -1), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("readings", kMonitorModes[mon_.meas].label,
                          ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        if (!monHistory_.empty()) {
            ImPlotSpec sp;
            sp.LineColor = ImVec4(0.35f, 1.0f, 0.5f, 1);
            sp.LineWeight = 1.5f;
            ImPlot::PlotLine("history", monHistory_.data(), int(monHistory_.size()), 1.0, 0.0, sp);
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
