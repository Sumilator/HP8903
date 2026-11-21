// SPDX-License-Identifier: GPL-2.0-only
//
// Tiny key=value settings file in the per-user config folder:
//   macOS   ~/Library/Application Support/HP8903/settings.ini
//   Windows %APPDATA%\HP8903\settings.ini
//   Linux   ~/.config/hp8903/settings.ini
#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace fs = std::filesystem;

inline fs::path configDir() {
#if defined(_WIN32)
    const char* a = std::getenv("APPDATA");
    return fs::path(a ? a : ".") / "HP8903";
#elif defined(__APPLE__)
    const char* h = std::getenv("HOME");
    return fs::path(h ? h : ".") / "Library" / "Application Support" / "HP8903";
#else
    const char* x = std::getenv("XDG_CONFIG_HOME");
    const char* h = std::getenv("HOME");
    return x ? fs::path(x) / "hp8903" : fs::path(h ? h : ".") / ".config" / "hp8903";
#endif
}

inline fs::path documentsDir() {
#if defined(_WIN32)
    const char* u = std::getenv("USERPROFILE");
    fs::path base = u ? fs::path(u) : fs::path(".");
#else
    const char* h = std::getenv("HOME");
    fs::path base = h ? fs::path(h) : fs::path(".");
#endif
    return base / "Documents" / "HP8903";
}

class Settings {
public:
    void load() {
        std::ifstream f(configDir() / "settings.ini");
        std::string line;
        while (std::getline(f, line)) {
            auto p = line.find('=');
            if (p != std::string::npos) kv_[line.substr(0, p)] = line.substr(p + 1);
        }
    }
    void save() const {
        std::error_code ec;
        fs::create_directories(configDir(), ec);
        std::ofstream f(configDir() / "settings.ini");
        for (auto& [k, v] : kv_) f << k << '=' << v << '\n';
    }
    std::string get(const std::string& k, const std::string& d) const {
        auto it = kv_.find(k);
        return it == kv_.end() ? d : it->second;
    }
    double num(const std::string& k, double d) const {
        auto it = kv_.find(k);
        return it == kv_.end() ? d : std::atof(it->second.c_str());
    }
    void set(const std::string& k, const std::string& v) { kv_[k] = v; }
    void set(const std::string& k, double v) { kv_[k] = std::to_string(v); }

private:
    std::map<std::string, std::string> kv_;
};
