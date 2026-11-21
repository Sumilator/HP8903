// SPDX-License-Identifier: GPL-2.0-only
//
// Tiny key=value settings file in the per-user config folder:
//   macOS   ~/Library/Application Support/HP8903/settings.ini
//   Windows %APPDATA%\HP8903\settings.ini
//   Linux   ~/.config/hp8903/settings.ini
#pragma once

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

// Plain string paths: std::filesystem needs macOS 10.15, we support 10.13.
#if defined(_WIN32)
constexpr char kSep = '\\';
#else
constexpr char kSep = '/';
#endif

inline std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    return (a.back() == '/' || a.back() == '\\') ? a + b : a + kSep + b;
}

inline std::string parentDir(const std::string& p) {
    auto i = p.find_last_of("/\\");
    return i == std::string::npos ? std::string(".") : p.substr(0, i);
}

// mkdir -p
inline bool makeDirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || ((path[i] == '/' || path[i] == '\\') && i > 0)) {
            cur = path.substr(0, i);
            if (cur.size() == 2 && cur[1] == ':') continue;   // "C:"
#if defined(_WIN32)
            int r = _mkdir(cur.c_str());
#else
            int r = mkdir(cur.c_str(), 0755);
#endif
            if (r != 0 && errno != EEXIST) return false;
        }
    }
    return true;
}

inline std::string configDir() {
#if defined(_WIN32)
    const char* a = std::getenv("APPDATA");
    return joinPath(a ? a : ".", "HP8903");
#elif defined(__APPLE__)
    const char* h = std::getenv("HOME");
    return joinPath(h ? h : ".", "Library/Application Support/HP8903");
#else
    const char* x = std::getenv("XDG_CONFIG_HOME");
    const char* h = std::getenv("HOME");
    return x ? joinPath(x, "hp8903") : joinPath(h ? h : ".", ".config/hp8903");
#endif
}

inline std::string documentsDir() {
#if defined(_WIN32)
    const char* u = std::getenv("USERPROFILE");
    return joinPath(joinPath(u ? u : ".", "Documents"), "HP8903");
#else
    const char* h = std::getenv("HOME");
    return joinPath(h ? h : ".", "Documents/HP8903");
#endif
}

class Settings {
public:
    void load() {
        std::ifstream f(joinPath(configDir(), "settings.ini"));
        std::string line;
        while (std::getline(f, line)) {
            auto p = line.find('=');
            if (p != std::string::npos) kv_[line.substr(0, p)] = line.substr(p + 1);
        }
    }
    void save() const {
        makeDirs(configDir());
        std::ofstream f(joinPath(configDir(), "settings.ini"));
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
