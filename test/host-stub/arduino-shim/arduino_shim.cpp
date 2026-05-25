// Runtime implementations for the Arduino host-test shim.

#include "Arduino.h"
#include "WiFi.h"
#include "WiFiClient.h"
#include "WiFiServer.h"
#include "FS.h"
#include "LittleFS.h"

#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fsys = std::filesystem;

// ---- globals ---------------------------------------------------------------
_HostSerial Serial;
_HostESP    ESP;
_HostWiFi   WiFi;
FS          LittleFS{"/tmp/_fugu_ftp_test"};

// ---- timing ----------------------------------------------------------------
static auto kBoot = std::chrono::steady_clock::now();

uint32_t millis() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - kBoot).count();
}
void delay(uint32_t ms)             { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void delayMicroseconds(uint32_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
void yield()                        { std::this_thread::yield(); }

// ---- WiFiClient ------------------------------------------------------------
WiFiClient::WiFiClient() = default;

WiFiClient::WiFiClient(int fd, IPAddress peer) {
    _s = new Shared{fd, 1, peer};
}

WiFiClient::WiFiClient(const WiFiClient &o)            { _acquire(o._s); }
WiFiClient &WiFiClient::operator=(const WiFiClient &o) {
    if (&o != this) { _release(); _acquire(o._s); }
    return *this;
}
WiFiClient::WiFiClient(WiFiClient &&o) noexcept            : _s(o._s) { o._s = nullptr; }
WiFiClient &WiFiClient::operator=(WiFiClient &&o) noexcept {
    if (&o != this) { _release(); _s = o._s; o._s = nullptr; }
    return *this;
}
WiFiClient::~WiFiClient() { _release(); }

void WiFiClient::_acquire(Shared *s) { _s = s; if (_s) _s->refs++; }
void WiFiClient::_release() {
    if (!_s) return;
    if (--_s->refs == 0) {
        if (_s->fd >= 0) ::close(_s->fd);
        delete _s;
    }
    _s = nullptr;
}

int WiFiClient::connect(IPAddress ip, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    sa.sin_addr.s_addr = htonl(((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16)
                              | ((uint32_t)ip[2] <<  8) |  (uint32_t)ip[3]);
    if (::connect(fd, (sockaddr *)&sa, sizeof(sa)) < 0) { ::close(fd); return 0; }
    _release();
    _s = new Shared{fd, 1, ip};
    return 1;
}
int WiFiClient::connect(const char *host, uint16_t port) {
    IPAddress ip;
    in_addr a;
    if (::inet_aton(host, &a) == 0) return 0;
    uint32_t v = ntohl(a.s_addr);
    ip = IPAddress((v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
    return connect(ip, port);
}

size_t WiFiClient::write(uint8_t b)                       { return write(&b, 1); }
size_t WiFiClient::write(const uint8_t *buf, size_t size) {
    if (!_s || _s->fd < 0) {
        std::fprintf(stderr, "  [shim] write to dead fd, size=%zu (%.20s)\n", size, (const char*)buf);
        return 0;
    }
    ssize_t n = ::send(_s->fd, buf, size, 0);
    if (n < 0) std::fprintf(stderr, "  [shim] send errno=%d fd=%d\n", errno, _s->fd);
    return n > 0 ? (size_t)n : 0;
}

int WiFiClient::available() {
    if (!_s || _s->fd < 0) return 0;
    int n = 0;
    if (::ioctl(_s->fd, FIONREAD, &n) < 0) return 0;
    return n;
}

int WiFiClient::read() {
    uint8_t b;
    int n = read(&b, 1);
    return n == 1 ? b : -1;
}
int WiFiClient::read(uint8_t *buf, size_t size) {
    if (!_s || _s->fd < 0) return -1;
    ssize_t n = ::recv(_s->fd, buf, size, MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if (n == 0) {  // peer closed
        ::close(_s->fd); _s->fd = -1;
        return -1;
    }
    return (int)n;
}
int WiFiClient::peek() {
    if (!_s || _s->fd < 0) return -1;
    uint8_t b;
    ssize_t n = ::recv(_s->fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
    return n == 1 ? (int)b : -1;
}
void WiFiClient::flush() {}

void WiFiClient::stop() {
    if (_s && _s->fd >= 0) { ::close(_s->fd); _s->fd = -1; }
}
uint8_t WiFiClient::connected() {
    if (!_s || _s->fd < 0) return 0;
    // Probe with MSG_PEEK; non-zero recv or EAGAIN means still connected.
    char tmp;
    ssize_t n = ::recv(_s->fd, &tmp, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n > 0) return 1;
    if (n == 0) { ::close(_s->fd); _s->fd = -1; return 0; }
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 1 : 0;
}
WiFiClient::operator bool() { return _s && _s->fd >= 0; }
IPAddress WiFiClient::remoteIP() { return _s ? _s->peer : IPAddress(); }

// ---- WiFiServer ------------------------------------------------------------
WiFiServer::WiFiServer(uint16_t port) : _port(port) {}
WiFiServer::~WiFiServer() { end(); }

void WiFiServer::begin(uint16_t port) {
    if (port) _port = port;
    if (_listenFd >= 0) return;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    // Non-blocking accept so handleFTP() doesn't stall.
    int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(_port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, (sockaddr *)&sa, sizeof(sa)) < 0) { ::close(fd); return; }
    if (::listen(fd, 4) < 0) { ::close(fd); return; }
    socklen_t sl = sizeof(sa);
    if (::getsockname(fd, (sockaddr *)&sa, &sl) == 0) _port = ntohs(sa.sin_port);
    _listenFd = fd;
}
void WiFiServer::end() {
    if (_listenFd >= 0) { ::close(_listenFd); _listenFd = -1; }
}

WiFiClient WiFiServer::accept() {
    if (_listenFd < 0) return WiFiClient();
    sockaddr_in sa{};
    socklen_t sl = sizeof(sa);
    int fd = ::accept(_listenFd, (sockaddr *)&sa, &sl);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            std::fprintf(stderr, "  [shim] accept errno=%d (%s)\n", errno, std::strerror(errno));
        return WiFiClient();
    }
    std::fprintf(stderr, "  [shim] accept got fd=%d\n", fd);
    uint32_t v = ntohl(sa.sin_addr.s_addr);
    IPAddress peer((v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
    return WiFiClient(fd, peer);
}
bool WiFiServer::hasClient() {
    if (_listenFd < 0) return false;
    fd_set rfds;
    FD_ZERO(&rfds); FD_SET(_listenFd, &rfds);
    timeval tv{0, 0};
    return ::select(_listenFd + 1, &rfds, nullptr, nullptr, &tv) > 0;
}

// ---- FS / File -------------------------------------------------------------
struct File::DirCursor {
    DIR *d = nullptr;
    std::string base;
    ~DirCursor() { if (d) ::closedir(d); }
};

File::File(std::FILE *fp, std::string path, bool isDir, std::uint64_t sz)
    : _fp(fp), _path(std::move(path)), _ok(true), _isDir(isDir), _size(sz) {
    auto slash = _path.find_last_of('/');
    _name = slash == std::string::npos ? _path : _path.substr(slash + 1);
    if (_isDir) {
        _cursor = std::make_shared<DirCursor>();
        _cursor->d = ::opendir(_path.c_str());
        _cursor->base = _path;
    }
}
File::File(File &&o) noexcept
    : _fp(o._fp), _path(std::move(o._path)), _name(std::move(o._name)),
      _ok(o._ok), _isDir(o._isDir), _size(o._size), _cursor(std::move(o._cursor)) {
    o._fp = nullptr; o._ok = false;
}
File &File::operator=(File &&o) noexcept {
    if (&o != this) {
        close();
        _fp = o._fp; _path = std::move(o._path); _name = std::move(o._name);
        _ok = o._ok; _isDir = o._isDir; _size = o._size; _cursor = std::move(o._cursor);
        o._fp = nullptr; o._ok = false;
    }
    return *this;
}
File::~File() { close(); }

void File::close() {
    if (_fp) { std::fclose(_fp); _fp = nullptr; }
    _cursor.reset();
    _ok = false;
}

int File::available() {
    if (!_fp) return 0;
    long pos = std::ftell(_fp);
    return (pos < 0 || (std::uint64_t)pos >= _size) ? 0 : (int)(_size - pos);
}
int File::read() { uint8_t b; return read(&b, 1) == 1 ? b : -1; }
int File::read(uint8_t *buf, size_t n) {
    if (!_fp) return -1;
    return (int)std::fread(buf, 1, n, _fp);
}
int File::peek() {
    if (!_fp) return -1;
    int c = std::fgetc(_fp);
    if (c == EOF) return -1;
    std::ungetc(c, _fp);
    return c;
}
size_t File::write(uint8_t b) { return write(&b, 1); }
size_t File::write(const uint8_t *buf, size_t n) {
    if (!_fp) return 0;
    return std::fwrite(buf, 1, n, _fp);
}
bool File::seek(uint32_t pos) {
    if (!_fp) return false;
    return std::fseek(_fp, (long)pos, SEEK_SET) == 0;
}

File File::openNextFile(const char *mode) {
    if (!_cursor || !_cursor->d) return File();
    dirent *e;
    while ((e = ::readdir(_cursor->d)) != nullptr) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
        std::string p = _cursor->base + "/" + e->d_name;
        struct stat st{};
        if (::stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) return File(nullptr, p, true, 0);
        std::FILE *fp = std::fopen(p.c_str(), mode);
        if (!fp) continue;
        return File(fp, p, false, (std::uint64_t)st.st_size);
    }
    return File();
}

// ---- FS --------------------------------------------------------------------
std::string FS::_resolve(const char *path) const {
    if (!path || !*path) return _root;
    if (path[0] == '/') return _root + path;
    return _root + "/" + path;
}

bool FS::begin(bool /*format*/) {
    std::error_code ec;
    fsys::create_directories(_root, ec);
    return !ec;
}

File FS::open(const char *path, const char *mode) {
    std::string p = _resolve(path);
    struct stat st{};
    bool exists = ::stat(p.c_str(), &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) return File(nullptr, p, true, 0);

    std::FILE *fp = std::fopen(p.c_str(), mode);
    if (!fp) return File();
    std::uint64_t sz = 0;
    if (::stat(p.c_str(), &st) == 0) sz = (std::uint64_t)st.st_size;
    return File(fp, p, false, sz);
}

bool FS::exists(const char *path)                      { struct stat st{}; return ::stat(_resolve(path).c_str(), &st) == 0; }
bool FS::remove(const char *path)                      { return ::unlink(_resolve(path).c_str()) == 0; }
bool FS::rename(const char *from, const char *to)      { return ::rename(_resolve(from).c_str(), _resolve(to).c_str()) == 0; }
bool FS::mkdir(const char *path)                       { return ::mkdir(_resolve(path).c_str(), 0777) == 0; }
bool FS::rmdir(const char *path)                       { return ::rmdir(_resolve(path).c_str()) == 0; }
