// Menu settings with file persistence (settings.cfg in the working directory).
//
// Keys (all optional, one per line, '#' starts a comment):
//   video.fullscreen = 0|1
//   video.show_fps   = 0|1
//   video.widescreen = 0|1
//
// A missing file keeps the defaults. A bad value warns on stderr and keeps
// that key's default; unknown keys are ignored so newer files stay readable.
// Saving rewrites the whole file and never fails the program: save() returns
// false and the caller warns.
#pragma once

#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

struct AppSettings {
    bool fullscreen = false;
    bool show_fps = false;
    bool widescreen = false;
};

namespace settings {

inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return "";
    const size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

// Parses one file line into the settings. Returns false (after warning) on a
// bad value; unknown keys are silently ignored.
inline bool apply_line(const std::string& path, int lineno, const std::string& name,
                       const std::string& value, AppSettings& s) {
    if (name == "video.fullscreen" || name == "video.show_fps" || name == "video.widescreen") {
        const std::string v = lower(value);
        bool b = false;
        if (v == "1" || v == "true" || v == "on" || v == "yes") {
            b = true;
        } else if (v == "0" || v == "false" || v == "off" || v == "no") {
            b = false;
        } else {
            std::fprintf(stderr, "%s:%d: bad boolean '%s', using default\n", path.c_str(), lineno,
                         value.c_str());
            return false;
        }
        if (name == "video.fullscreen") {
            s.fullscreen = b;
        } else if (name == "video.show_fps") {
            s.show_fps = b;
        } else {
            s.widescreen = b;
        }
        return true;
    }
    return true;
}

// Missing file keeps the defaults and returns true.
inline bool load(const std::string& path, AppSettings& s) {
    std::ifstream f(path);
    if (!f) return true;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        lineno++;
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) {
            std::fprintf(stderr, "%s:%d: expected name = value, skipping\n", path.c_str(), lineno);
            continue;
        }
        apply_line(path, lineno, trim(t.substr(0, eq)), trim(t.substr(eq + 1)), s);
    }
    return true;
}

inline bool save(const std::string& path, const AppSettings& s) {
    // Line-preserving update: the file also holds controller bindings, so
    // only the known keys are rewritten in place and everything else
    // (bindings, comments, blank lines) is kept as-is.
    const char* names[3] = {"video.fullscreen", "video.show_fps", "video.widescreen"};
    std::string values[3] = {
        s.fullscreen ? "1" : "0",
        s.show_fps ? "1" : "0",
        s.widescreen ? "1" : "0",
    };
    std::vector<std::string> lines;
    {
        std::ifstream f(path);
        std::string line;
        while (f && std::getline(f, line)) lines.push_back(line);
    }
    bool seen[3] = {false, false, false};
    std::string out;
    for (const std::string& line : lines) {
        const std::string t = trim(line);
        bool known = false;
        if (!t.empty() && t[0] != '#') {
            const size_t eq = t.find('=');
            if (eq != std::string::npos) {
                const std::string name = trim(t.substr(0, eq));
                for (int i = 0; i < 3; i++) {
                    if (name == names[i]) {
                        known = true;
                        if (!seen[i]) {
                            out += names[i];
                            out += " = ";
                            out += values[i];
                            out += '\n';
                            seen[i] = true;
                        }
                    }
                }
            }
        }
        if (!known) {
            out += line;
            out += '\n';
        }
    }
    if (lines.empty()) {
        out = "# smb3pc settings. Written by the in-game menu; edit by hand if you like.\n" + out;
    }
    for (int i = 0; i < 3; i++) {
        if (!seen[i]) {
            out += names[i];
            out += " = ";
            out += values[i];
            out += '\n';
        }
    }
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << out;
    return static_cast<bool>(f);
}

}  // namespace settings
