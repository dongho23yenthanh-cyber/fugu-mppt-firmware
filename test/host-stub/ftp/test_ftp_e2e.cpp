// Host-side end-to-end regression for the four SimpleFTPServer security fixes
// (PASS-without-USER bypass, makePath stack overflow, PORT NULL deref, PORT
// FTP-bounce). Compiles the vendored FtpServer.cpp against the reusable
// Arduino shim in ../arduino-shim, runs it on loopback ports, and drives it
// from the same process via raw TCP.
//
// Each test runs in a forked subprocess so a crash (the original PORT NULL
// deref would SIGSEGV the server) reports as a clean test failure instead of
// taking out the entire binary.
//
// Build (or use test/host-stub/ftp/CMakeLists.txt):
//   clang++ -std=gnu++17 -fexceptions -Wno-overloaded-virtual \
//     -I test/host-stub/arduino-shim -I components/SimpleFTPServer/SimpleFTPServer \
//     -DESP32 -DFTP_SERVER_NETWORK_TYPE=6 -DSTORAGE_TYPE=7 \
//     -DDEFAULT_FTP_SERVER_NETWORK_TYPE_ESP32=6 -DDEFAULT_STORAGE_TYPE_ESP32=7 \
//     -DESP_ARDUINO_VERSION_MAJOR=3 \
//     test/host-stub/arduino-shim/arduino_shim.cpp \
//     components/SimpleFTPServer/SimpleFTPServer/FtpServer.cpp \
//     test/host-stub/ftp/test_ftp_e2e.cpp \
//     -o /tmp/ftp-e2e-test && /tmp/ftp-e2e-test

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "Arduino.h"
#include "FS.h"
#include "LittleFS.h"
#include "WiFi.h"
#include "WiFiServer.h"

// Pin the vendored lib's flavor before including its header.
#define FTP_SERVER_NETWORK_TYPE 6  // NETWORK_ESP32
#define STORAGE_TYPE            7  // STORAGE_LITTLEFS
#include "FtpServer.h"

namespace {

constexpr uint16_t kCmdPort  = 12121;
constexpr uint16_t kPasvPort = 12122;
constexpr const char *USER = "ftp";
constexpr const char *PASS = "secret";

FtpServer server(kCmdPort, kPasvPort);
std::atomic<bool> running{true};

void ftpLoop() {
    while (running.load(std::memory_order_relaxed)) {
        server.handleFTP();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Tiny FTP client over raw TCP. readCode() returns the 3-digit code of the
// next complete reply, honoring RFC 959 multi-line continuations
// ("ddd-..." lines until "ddd ...").
struct FtpDial {
    int fd = -1;
    std::string buf;

    explicit FtpDial(uint16_t port) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int rc = ::connect(fd, (sockaddr *)&sa, sizeof(sa));
        assert(rc == 0 && "connect to FTP control socket failed");
        // Bound each blocking recv so a missing reply surfaces as -1 instead
        // of hanging the test.
        timeval tv{2, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    ~FtpDial() { if (fd >= 0) ::close(fd); }

    void send(const std::string &line) {
        std::string s = line + "\r\n";
        ssize_t n = ::send(fd, s.data(), s.size(), 0);
        (void)n;
    }

    int readCode() {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        int lastCode = -1;
        while (std::chrono::steady_clock::now() < deadline) {
            for (;;) {
                auto nl = buf.find('\n');
                if (nl == std::string::npos) break;
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (line.size() >= 4) {
                    int code = std::atoi(line.substr(0, 3).c_str());
                    char sep = line[3];
                    if (code > 0) {
                        lastCode = code;
                        if (sep == ' ') return code;
                    }
                }
            }
            char tmp[256];
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n > 0) buf.append(tmp, tmp + (size_t)n);
            else if (n == 0) {
                // Peer closed. If buf holds a terminator-less single line
                // (e.g. "221 Goodbye" lost trailing \r\n on close), parse it.
                if (lastCode == -1 && buf.size() >= 4) {
                    int code = std::atoi(buf.substr(0, 3).c_str());
                    if (code > 0 && buf[3] == ' ') return code;
                }
                break;
            }
            // -1 with EAGAIN: SO_RCVTIMEO fired; loop until outer deadline.
        }
        return lastCode;
    }

    int login() {
        if (readCode() != 220) return -1;
        send(std::string("USER ") + USER);
        if (readCode() != 331) return -1;
        send(std::string("PASS ") + PASS);
        return readCode();
    }
};

// ---- per-bug regressions ---------------------------------------------------

// Bug #1: PASS-without-USER bypass. The buggy code printed 503 *and* fell
// through to authenticate if the password matched — meaning the server queues
// "503 ..." and "230 Ok" back-to-back. We must drain any 2xx that arrives
// before sending a follow-up command, then check that PWD doesn't succeed.
void test_pass_without_user() {
    FtpDial c(kCmdPort);
    assert(c.readCode() == 220);

    c.send(std::string("PASS ") + PASS);

    // Read all replies the server sends in response to the bare PASS. On the
    // patched code this is exactly one (503). On buggy code there are two
    // (503 then 230). A 230 here directly proves the bypass.
    int first  = c.readCode();
    int second = c.readCode();
    std::fprintf(stderr, "  PASS -> first=%d, second=%d (patched: 503/-1; buggy: 503/230)\n",
                 first, second);
    assert(first == 503 && "PASS without USER must be rejected with 503");
    assert(second != 230 && "PASS-before-USER must NOT also send 230 (bypass)");

    // Belt-and-braces: a follow-up PWD must NOT succeed (257).
    c.send("PWD");
    int pwdCode = c.readCode();
    std::fprintf(stderr, "  PWD->%d (must NOT be 257)\n", pwdCode);
    assert(pwdCode != 257 && "PASS-before-USER must not authenticate the session");
}

// Bug #2: makePath stack overflow. The buggy strncat overflows fullName when
// workingDir + "/" + param > FTP_CWD_SIZE (263 B). Setup: mkdir a long dir,
// CWD into it (makes workingDir long), then CWD a relative path long enough
// to overflow the 263-byte buffer but short enough that the lib's per-line
// cmd buffer (also FTP_CMD_SIZE=263) accepts it.
void test_long_cwd_path() {
    FtpDial c(kCmdPort);
    assert(c.login() == 230);

    // 100-char base dir → workingDir becomes "/" + 100 chars = 101 B.
    std::string base(100, 'B');
    c.send("MKD /" + base);  (void)c.readCode();  // 257 or 550 — don't care
    c.send("CWD /" + base);
    int cwdOk = c.readCode();
    std::fprintf(stderr, "  CWD long base ->%d\n", cwdOk);
    assert((cwdOk == 250 || cwdOk == 257) && "setup CWD failed");

    // 200-char relative arg fits in cmd line ("CWD " + 200 = 204 < 263) but
    // combined with the 101-B workingDir blows the 263-B path buffer.
    std::string huge(200, 'A');
    c.send("CWD " + huge);
    int code = c.readCode();
    std::fprintf(stderr, "  CWD long arg ->%d (expect 500)\n", code);
    assert(code == 500 && "long CWD must be rejected with 500, not overflow");

    c.send("NOOP");
    int noop = c.readCode();
    std::fprintf(stderr, "  NOOP->%d (server must stay alive)\n", noop);
    assert(noop == 200 && "server must remain responsive after long CWD");
}

// Bug #3: PORT NULL deref. "PORT 1" crashed the server. Patched: 501.
void test_malformed_port() {
    FtpDial c(kCmdPort);
    assert(c.login() == 230);

    c.send("PORT 1");
    int code = c.readCode();
    std::fprintf(stderr, "  PORT 1->%d (expect 501)\n", code);
    assert(code == 501 && "malformed PORT must return 501, not crash");

    c.send("NOOP");
    assert(c.readCode() == 200);
}

// Bug #4: FTP-bounce. PORT to a non-peer IP must be refused.
void test_port_bounce() {
    FtpDial c(kCmdPort);
    assert(c.login() == 230);

    c.send("PORT 192,168,1,1,0,21");
    int code = c.readCode();
    std::fprintf(stderr, "  PORT 192.168.1.1->%d (expect 501)\n", code);
    assert(code == 501 && "PORT to non-peer IP must be refused");
}

// ---- subprocess harness ---------------------------------------------------

struct Test {
    const char *name;
    void (*fn)();
};

// Fork-per-test so an assertion (or, for buggy code, SIGSEGV in the server
// thread shared by all subsequent tests) is reported as a single failure
// instead of taking down later tests too. Server runs in the parent.
bool runForked(const Test &t) {
    pid_t pid = ::fork();
    if (pid < 0) { std::perror("fork"); return false; }
    if (pid == 0) {
        t.fn();
        std::_Exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        std::fprintf(stderr, "  PASS %s\n\n", t.name);
        return true;
    }
    if (WIFSIGNALED(status)) {
        std::fprintf(stderr, "  FAIL %s — killed by signal %d\n\n", t.name, WTERMSIG(status));
    } else {
        std::fprintf(stderr, "  FAIL %s — exit %d\n\n", t.name, WEXITSTATUS(status));
    }
    return false;
}

}  // namespace

int main() {
    // Writing to a half-closed peer would otherwise SIGPIPE the whole binary.
    std::signal(SIGPIPE, SIG_IGN);

    LittleFS.begin(true);
    server.begin(USER, PASS);

    std::thread t(ftpLoop);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const Test tests[] = {
        // long_cwd_path is destructive on buggy code (stack smash in the
        // server thread crashes the parent process too). Run it last so
        // the other regressions still report cleanly on a buggy build.
        {"pass_without_user", test_pass_without_user},
        {"malformed_port",    test_malformed_port},
        {"port_bounce",       test_port_bounce},
        {"long_cwd_path",     test_long_cwd_path},
    };

    int failures = 0;
    for (const auto &t : tests) {
        std::fprintf(stderr, "--- %s ---\n", t.name);
        if (!runForked(t)) ++failures;
        // SimpleFTPServer is single-client; let the server reset to listening
        // before the next test connects.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    running = false;
    t.join();

    if (failures) {
        std::fprintf(stderr, "%d test(s) FAILED\n", failures);
        return 1;
    }
    std::fprintf(stderr, "ALL FTP REGRESSION TESTS PASSED\n");
    return 0;
}
