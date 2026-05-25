#pragma once
#include "Arduino.h"

#include <cstdio>
#include <memory>
#include <string>

// Open-mode tokens used by Arduino FS APIs.
#ifndef FILE_READ
#define FILE_READ  "r"
#endif
#ifndef FILE_WRITE
#define FILE_WRITE "w"
#endif
#ifndef FILE_APPEND
#define FILE_APPEND "a"
#endif

namespace fs {
class File;
}

// Minimal File: backed by a real on-disk file under the tmpdir managed by
// the host LittleFS shim. Supports just enough for SimpleFTPServer to walk
// CWD/LIST/RETR/STOR paths.
class File : public Stream {
public:
    File() = default;
    File(std::FILE *fp, std::string path, bool isDir, std::uint64_t size);
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    File(File &&) noexcept;
    File &operator=(File &&) noexcept;
    ~File() override;

    // Implicit on purpose: vendored lib does `if (f == true)`, which requires
    // an implicit File→bool conversion.
    operator bool() const { return _ok; }
    bool        isDirectory() const { return _isDir; }
    const char *name() const        { return _name.c_str(); }
    std::uint64_t size() const      { return _size; }
    // Used by LIST/MLSD format builders. Real mtime not needed for the
    // command-handling regression tests.
    uint32_t    getLastWrite() const { return 0; }

    int    available() override;
    int    read() override;
    int    read(uint8_t *buf, size_t n);
    int    peek() override;
    size_t write(uint8_t b) override;
    size_t write(const uint8_t *buf, size_t n) override;
    using Print::write;
    bool   seek(uint32_t pos);
    void   close();

    // Directory iteration. Returns an "empty" File when exhausted.
    File openNextFile(const char *mode = "r");

private:
    std::FILE  *_fp     = nullptr;
    std::string _path;
    std::string _name;
    bool        _ok     = false;
    bool        _isDir  = false;
    std::uint64_t _size = 0;

    // Directory iteration cursor (only valid if _isDir).
    struct DirCursor;
    std::shared_ptr<DirCursor> _cursor;
};

class FS {
public:
    explicit FS(std::string root) : _root(std::move(root)) {}

    bool   begin(bool format = false);
    File   open(const char *path, const char *mode = "r");
    File   open(const String &path, const char *mode = "r") { return open(path.c_str(), mode); }
    bool   exists(const char *path);
    bool   remove(const char *path);
    bool   rename(const char *from, const char *to);
    bool   mkdir(const char *path);
    bool   rmdir(const char *path);
    std::uint64_t totalBytes() { return 1024 * 1024; }
    std::uint64_t usedBytes()  { return 0; }

    // Test helpers
    const std::string &root() const { return _root; }
    void setRoot(std::string r)     { _root = std::move(r); }

private:
    std::string _root;
    std::string _resolve(const char *path) const;
};
