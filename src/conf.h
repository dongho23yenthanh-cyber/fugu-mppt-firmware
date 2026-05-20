#pragma once


#include <cstdio>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include<unordered_map>
#include <numeric>
#include <functional>
#include <vector>
#include <limits>
#include <cmath> // std::isfinite (getFloat warnIfNan branch)
#include <cstring>
#include <algorithm> // std::transform
#include <esp_log.h>

#include "console.h"


#define TAG "conf"

inline std::string trim(const std::string &s) {
    // removes whitespace characters from beginnig and end of string s
    const int l = (int) s.length();
    int a = 0, b = l - 1;
    char c;
    while (a < l && ((c = s[a]) == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' || c == '\0'))
        a++;
    while (b > a && ((c = s[b]) == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' || c == '\0'))
        b--;
    return s.substr(a, 1 + b - a);
}

class ConfFile {
    std::unordered_map<std::string, std::string> _map;
    const char *path;

public:
    // In-memory ConfFile, primarily for tests. Skips the file read.
    explicit ConfFile() : _map({}), path("") {}
    explicit ConfFile(std::unordered_map<std::string, std::string> map ) : _map(std::move(map)), path("<in-mem>") {}

    explicit ConfFile(const char *path, bool no_warn_if_not_open = false) : path(path) {
        FILE *f = fopen(path, "r");
        if (!f) {
            if (!no_warn_if_not_open) {
                ESP_LOGW(TAG, "cannot read ConfFile %s", path);
            }
            return;
        }
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) {
            std::string line(buf);
            auto ic = line.find_first_of('#');
            if (ic != std::string::npos)
                line = line.substr(0, ic);
            line = trim(line);
            if (line.length() == 0)
                continue;
            auto ie = line.find_first_of('=');
            if (ie == std::string::npos) {
                // tolerate a malformed line: skip it and keep the rest of the file usable, rather
                // than throwing (an uncaught throw here at boot would reboot → re-read → boot loop)
                ESP_LOGW(TAG, "skipping malformed line in %s: '=' not found in '%s'", path, line.c_str());
                continue;
            }
            auto k = trim(line.substr(0, ie));
            if (_map.find(k) != _map.end()) {
                ESP_LOGW(TAG, "duplicate key %s in file '%s'", k.c_str(), path);
            }
            _map[k] = trim(line.substr(ie + 1));
        }
        fclose(f);
    }


    // Update keys in place and rewrite the whole file, so repeated writes never grow it with
    // duplicate lines. Existing keys keep their position; their inline comment (`# ...` after the
    // value) is preserved. Comment-only lines and blank lines are kept verbatim. Pre-existing
    // duplicate lines of a key being written are collapsed to one. Genuinely new keys are appended.
    // Returns false on any I/O failure (cannot open / short write / fsync error) instead of
    // aborting — a failed config write must never panic-reboot the device.
    bool add(const std::unordered_map<std::string, std::string> &values, bool overwrite = false) {
        // read current file into lines (stripping the trailing newline)
        std::vector<std::string> lines;
        if (FILE *fr = fopen(path, "r")) {
            char buf[256];
            while (fgets(buf, sizeof(buf), fr)) {
                std::string line(buf);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                    line.pop_back();
                lines.push_back(line);
            }
            fclose(fr);
        }

        std::unordered_map<std::string, bool> written;
        for (auto &[key, val]: values) written[key] = false;

        std::vector<std::string> out;
        out.reserve(lines.size() + values.size());

        for (auto &line: lines) {
            auto ic = line.find_first_of('#');
            std::string code = (ic == std::string::npos) ? line : line.substr(0, ic);
            std::string comment = (ic == std::string::npos) ? std::string{} : line.substr(ic);

            auto ie = code.find_first_of('=');
            if (ie == std::string::npos) {
                out.push_back(line); // comment-only or blank line
                continue;
            }

            auto key = trim(code.substr(0, ie));
            auto it = values.find(key);
            if (it == values.end()) {
                out.push_back(line); // a key we're not touching
                continue;
            }

            if (!overwrite)
                throw std::runtime_error("duplicate key: " + key);

            if (written[key]) {
                ESP_LOGW(TAG, "collapsing duplicate key %s in %s", key.c_str(), path);
                continue; // drop redundant duplicate line
            }

            std::string nl = key + "=" + it->second;
            if (!comment.empty()) nl += "  " + comment; // keep the inline comment
            out.push_back(nl);
            written[key] = true;
        }

        for (auto &[key, val]: values)
            if (!written[key]) out.push_back(key + "=" + val);

        FILE *f = fopen(path, "w");
        if (f == nullptr) {
            ESP_LOGE("store", "Cannot write %s", path);
            return false;
        }
        for (auto &line: out) {
            if (!line.empty() && fwrite(line.c_str(), line.length(), 1, f) != 1) {
                ESP_LOGE("store", "short write to %s", path);
                fclose(f);
                return false;
            }
            fputc('\n', f);
        }

#ifndef CONFIG_LITTLEFS_FLUSH_FILE_EVERY_WRITE
        if (fsync(fileno(f)) != 0) {
            ESP_LOGE("store", "fsync failed for %s", path);
            fclose(f);
            return false;
        }
#endif

        fclose(f);

        for (auto &[key, val]: values) _map[key] = val; // keep in-memory view consistent
        return true;
    }

    // Append-only insert: just tacks `key=val` onto the end without reading/rewriting the file.
    // Faster than add() (one open + sequential write, no parse), but it does NOT update existing
    // keys in place, so re-writing the same key GROWS the file with duplicate lines (last one wins
    // on read). Use only for one-shot writes / append-heavy paths where file growth is acceptable;
    // prefer add() for keys that get rewritten repeatedly (e.g. service enabled/log_level).
    // Returns false on any I/O failure instead of aborting (see add()).
    bool addFast(const std::unordered_map<std::string, std::string> &values, bool overwrite = false) {
        FILE *f = fopen(path, "a");
        if (f == nullptr) {
            f = fopen(path, "w");
            if (f == nullptr) {
                ESP_LOGE("store", "Cannot write %s", path);
                return false;
            }
        }

        for (auto &[key, val]: values) {
            if (_map.find(key) != _map.end()) {
                if (!overwrite) {
                    fclose(f);
                    throw std::runtime_error("duplicate key: " + key);
                }
                ESP_LOGW(TAG, "addFast duplicate key %s (file grows)", key.c_str());
            }
            fputc('\n', f);
            if (fwrite(key.c_str(), key.length(), 1, f) != 1
                || fwrite("=", 1, 1, f) != 1
                || fwrite(val.c_str(), val.length(), 1, f) != 1) {
                ESP_LOGE("store", "short write to %s", path);
                fclose(f);
                return false;
            }
        }

#ifndef CONFIG_LITTLEFS_FLUSH_FILE_EVERY_WRITE
        if (fsync(fileno(f)) != 0) {
            ESP_LOGE("store", "fsync failed for %s", path);
            fclose(f);
            return false;
        }
#endif

        fclose(f);

        for (auto &[key, val]: values) _map[key] = val; // keep in-memory view consistent
        return true;
    }

    template<typename T>
    // # (const char *, char **)
    T getX(const std::string &key, T def, const std::function<T(const char *, char **)> &strto_,
           bool noDef = false) const {
        // strto_ error handling https://stackoverflow.com/questions/26080829/detecting-strtol-failure
        auto i = _map.find(key);
        if (i != _map.end()) {
            char *endptr = nullptr;
            errno = 0; // reset
            T l = strto_(i->second.c_str(), &endptr);
            if (errno != 0) {
                ESP_LOGE(TAG, "%s:%s: strto_(\"%s\") failed: ret=%f, errno=%i", path, key.c_str(), i->second.c_str(),
                         (float) l, errno);
                throw std::runtime_error("strto_ error " + i->second);
            }
            if (*endptr != 0) {
                ESP_LOGE(TAG, "%s:%s additional chars after strtol(%s): '%s'", path, key.c_str(), i->second.c_str(),
                         endptr);
                //assert(false);
                throw std::runtime_error("additional chars " + i->second);
            }
            return l;
        }

        if (!noDef && def == std::numeric_limits<T>::max()) {
            auto v = keys();
            std::string s = std::accumulate(v.begin(), v.end(), std::string{});
            ESP_LOGE(TAG, "key '%s' not found in %s (%s)", key.c_str(), s.c_str(), path);
            //assert(false);
            throw std::runtime_error("key not found: " + key);
        }

        return def;
    }

    std::vector<std::string> keys() const {
        std::vector<std::string> keys{_map.size()};
        std::transform(_map.begin(), _map.end(), keys.begin(),
                       [](const std::pair<std::string, std::string> &p) { return p.first; });
        return keys;
    }

    //inline static long strtol_10(const char *s, char **endptr) { return strtol(s, endptr, 10); }

    inline static long strtol_2_8_10_16(const char *s, char **endptr) {
        int off = 0, base = 10;
        auto len = strlen(s);

        if (len > 2 and strncmp(s, "0b", 2) == 0) {
            base = 2;
            off = 2;
        } else if (len > 2 and strncmp(s, "0x", 2) == 0) {
            base = 16;
            off = 2;
        } else if (len > 1 && s[0] == '0' && strchr(s, '.') == nullptr && strchr(s, 'e') == nullptr) {
            // valid floats (not octal): 0.1, 01e1
            off = 1;
            base = 8;
        }
        return strtol(s + off, endptr, base);
    }

    long getLong(const std::string &key, long def = std::numeric_limits<long>::max()) const {
        return getX<long>(key, def, strtol_2_8_10_16);
    }

    uint8_t getByte(const std::string &key) const {
        return getX<long>(key, 255, strtol_2_8_10_16, true);
    }

    uint8_t getByte(const std::string &key, uint8_t def) const {
        return getX<long>(key, def, strtol_2_8_10_16);
    }

    long getLong(const std::string &key, int base, long def) {
        auto fn = [base](const char *s, char **endptr) { return std::strtol(s, endptr, base); };
        return getX<long>(key, def, fn);
    }

    float getFloat(const std::string &key, float def = std::numeric_limits<float>::max(),
                   bool warnIfNan = false) const {
        auto v = getX<float>(key, def, std::strtof);
        if (warnIfNan and !std::isfinite(v))
            LOG_VALUE_NOT_FINITE("conf", key.c_str(), path);
        return v;
    }

    float f(const std::string &key, float def = std::numeric_limits<float>::max()) { return getFloat(key, def); }

    const std::string &getString(const std::string &key) const {
        auto i = _map.find(key);
        if (i != _map.end())
            return i->second;
        throw std::runtime_error("key not found: " + key);
    }

    [[nodiscard]] const std::string &getString(const std::string &key, const std::string &def) const {
        auto i = _map.find(key);
        if (i != _map.end()) {
            return i->second;
        }
        return def;
        //ESP_LOGE(TAG, "key '%s' not found", key.c_str());
        //assert(false);
    }

    const char *c(const std::string &key, const char *def = nullptr) {
        auto i = _map.find(key);
        if (i != _map.end()) {
            return i->second.c_str();
        }
        return def;
    }

    explicit operator bool() const { return !_map.empty(); }
};

#undef TAG
