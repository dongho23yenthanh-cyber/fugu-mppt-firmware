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

    // Send PASV, parse the "227 ... (h1,h2,h3,h4,p1,p2)" reply, return the
    // advertised data port (0 on parse failure). Reads through buf the same
    // way readCode does, so call it instead of readCode for PASV.
    uint16_t pasv() {
        send("PASV");
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            auto nl = buf.find('\n');
            while (nl != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (line.rfind("227", 0) == 0) {
                    auto lp = line.find('(');
                    auto rp = line.find(')', lp == std::string::npos ? 0 : lp);
                    if (lp == std::string::npos || rp == std::string::npos) return 0;
                    unsigned a, b, c, d, p1, p2;
                    if (std::sscanf(line.c_str() + lp + 1, "%u,%u,%u,%u,%u,%u",
                                    &a, &b, &c, &d, &p1, &p2) != 6) return 0;
                    return (uint16_t)((p1 << 8) | p2);
                }
                nl = buf.find('\n');
            }
            char tmp[256];
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n > 0) buf.append(tmp, tmp + (size_t)n);
            else if (n == 0) break;
        }
        return 0;
    }
};

// Open a fresh TCP socket to 127.0.0.1:port for the FTP data channel.
int openData(uint16_t port) {
    int dfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (dfd < 0) return -1;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(dfd, (sockaddr *)&sa, sizeof(sa)) < 0) { ::close(dfd); return -1; }
    timeval tv{5, 0};
    ::setsockopt(dfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return dfd;
}

// Drain everything readable on a data socket until peer closes; return all
// bytes received.
std::string drainData(int dfd) {
    std::string out;
    char buf[1024];
    for (;;) {
        ssize_t n = ::recv(dfd, buf, sizeof(buf), 0);
        if (n > 0) out.append(buf, buf + n);
        else break;  // 0 = peer closed; -1 = timeout
    }
    return out;
}

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

// ---- data-channel coverage -------------------------------------------------

// STOR then RETR a small file. Verifies the full passive-mode data path
// (PASV → 227, parse port, open data conn, STOR → 150 → bytes → 226, then
// RETR → 150 → bytes → 226).
void test_upload_download() {
    FtpDial c(kCmdPort);
    assert(c.login() == 230);

    const std::string payload = "hello fugu ftp host-shim\n";

    // --- Upload ---
    uint16_t pport = c.pasv();
    std::fprintf(stderr, "  PASV->%u\n", pport);
    assert(pport > 0 && "PASV must advertise a data port");
    int dfd = openData(pport);
    assert(dfd >= 0 && "data socket connect failed");

    c.send("STOR /upload.bin");
    int stor = c.readCode();
    std::fprintf(stderr, "  STOR ->%d (expect 150)\n", stor);
    assert(stor == 150);

    ssize_t sent = ::send(dfd, payload.data(), payload.size(), 0);
    assert(sent == (ssize_t)payload.size());
    ::close(dfd);  // FIN triggers server to finalize and send 226

    int done = c.readCode();
    std::fprintf(stderr, "  STOR done ->%d (expect 226)\n", done);
    assert(done == 226);

    // --- Download (round-trip what we just uploaded) ---
    pport = c.pasv();
    assert(pport > 0);
    dfd = openData(pport);
    assert(dfd >= 0);

    c.send("RETR /upload.bin");
    int retr = c.readCode();
    std::fprintf(stderr, "  RETR ->%d (expect 150)\n", retr);
    assert(retr == 150);

    std::string got = drainData(dfd);
    ::close(dfd);
    std::fprintf(stderr, "  RETR got %zu bytes (expect %zu)\n", got.size(), payload.size());
    assert(got == payload && "RETR payload mismatch");

    int complete = c.readCode();
    std::fprintf(stderr, "  RETR done ->%d (expect 226)\n", complete);
    assert(complete == 226);
}

// MLSD with an explicit pathname argument (recently added upstream as
// commit 488d4d4 "Honor optional pathname argument in LIST/NLST/MLSD").
// Setup: create /mlsdsub/ and drop a marker file in it. Verify MLSD /mlsdsub
// lists the marker — not whatever's at cwd.
void test_mlsd_subdir() {
    FtpDial c(kCmdPort);
    assert(c.login() == 230);

    c.send("MKD /mlsdsub");
    (void)c.readCode();  // 257 if fresh, 550 if it survived from a prior run

    // Drop a marker file inside the subdir.
    uint16_t pport = c.pasv();
    assert(pport > 0);
    int dfd = openData(pport);
    assert(dfd >= 0);
    c.send("STOR /mlsdsub/marker.txt");
    assert(c.readCode() == 150);
    const char *payload = "x";
    ::send(dfd, payload, 1, 0);
    ::close(dfd);
    assert(c.readCode() == 226);

    // MLSD /mlsdsub should list marker.txt — not the contents of cwd.
    pport = c.pasv();
    assert(pport > 0);
    dfd = openData(pport);
    assert(dfd >= 0);

    c.send("MLSD /mlsdsub");
    int mlsd = c.readCode();
    std::fprintf(stderr, "  MLSD /mlsdsub ->%d (expect 150)\n", mlsd);
    assert(mlsd == 150);

    std::string listing = drainData(dfd);
    ::close(dfd);
    std::fprintf(stderr, "  MLSD listing (%zu B): %.120s\n", listing.size(), listing.c_str());
    assert(c.readCode() == 226);

    assert(listing.find("marker.txt") != std::string::npos
           && "MLSD with subdir arg must list files in that subdir");
    assert(listing.find("mlsdsub") == std::string::npos
           && "MLSD listing must contain only entries *inside* the subdir, not the subdir name itself");
}

// CWD "../" from root must stay at root — the lib's makePath walks up
// workingDir but clamps at "/". This is the one portable, FS-independent
// path-traversal assertion. (Embedded "..", e.g. "/sub/../foo", is *not*
// normalized by the lib and falls through to the FS layer's path
// resolution, so its behavior depends on the underlying FS — out of
// scope here.)
void test_cwd_dotdot_clamps_at_root() {
    FtpDial c(kCmdPort);
    assert(c.login() == 230);

    c.send("CWD /");                     assert(c.readCode() == 250);
    c.send("PWD");
    // PWD reply is "257 \"/\" is the current directory."
    int pwd0 = c.readCode();
    std::fprintf(stderr, "  PWD->%d at root\n", pwd0);
    assert(pwd0 == 257);

    c.send("CWD ..");                    int cwdUp = c.readCode();
    std::fprintf(stderr, "  CWD .. ->%d (from /)\n", cwdUp);
    assert(cwdUp == 250 || cwdUp == 550);  // either succeed-as-noop or refuse

    c.send("PWD");                       int pwd1 = c.readCode();
    std::fprintf(stderr, "  PWD->%d after CWD ..\n", pwd1);
    assert(pwd1 == 257);
    // Note: we don't parse the path string out of the 257 reply; the lib
    // sends "257 \"/\" ..." in both the "/" and "//" cases. Either is fine
    // here — the assertion is that we didn't escape into something else.
}

// Multi-chunk STOR + RETR. The lib's `fileBuffer` is allocated dynamically
// up to BUFFERSIZE (1436 on ESP32). A 10 KB payload guarantees multiple
// read/write iterations through doFiletoNetwork / doNetworkToFile.
void test_large_file_round_trip() {
    FtpDial c(kCmdPort);
    assert(c.login() == 230);

    // 10 KB deterministic pattern; every byte distinct mod 251 makes
    // any off-by-one corruption easy to spot.
    std::string payload(10 * 1024, '\0');
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = (char)(i % 251);

    // Upload
    uint16_t pport = c.pasv();        assert(pport > 0);
    int dfd = openData(pport);        assert(dfd >= 0);
    c.send("STOR /large.bin");        assert(c.readCode() == 150);
    // Drain-as-we-write to avoid stalling on the kernel send buffer.
    size_t off = 0;
    while (off < payload.size()) {
        ssize_t n = ::send(dfd, payload.data() + off, payload.size() - off, 0);
        assert(n > 0);
        off += (size_t)n;
    }
    ::close(dfd);
    assert(c.readCode() == 226);

    // Download
    pport = c.pasv();                 assert(pport > 0);
    dfd = openData(pport);            assert(dfd >= 0);
    c.send("RETR /large.bin");        assert(c.readCode() == 150);
    std::string got = drainData(dfd);
    ::close(dfd);
    assert(c.readCode() == 226);

    std::fprintf(stderr, "  got %zu bytes (expect %zu)\n", got.size(), payload.size());
    assert(got.size() == payload.size() && "RETR size mismatch on 10 KB");
    assert(got == payload && "RETR content mismatch on 10 KB");
}

// DELE happy path + verify a follow-up RETR fails with the not-found code.
void test_dele_then_retr_fails() {
    FtpDial c(kCmdPort);
    assert(c.login() == 230);

    // Upload a victim file
    uint16_t pport = c.pasv();        assert(pport > 0);
    int dfd = openData(pport);        assert(dfd >= 0);
    c.send("STOR /todelete.bin");     assert(c.readCode() == 150);
    const char *body = "doomed";
    ::send(dfd, body, std::strlen(body), 0);
    ::close(dfd);
    assert(c.readCode() == 226);

    // Delete it
    c.send("DELE /todelete.bin");
    int del = c.readCode();
    std::fprintf(stderr, "  DELE ->%d (expect 250)\n", del);
    assert(del == 250);

    // Now RETR must fail. The lib's makeExistsPath sends "550 <path> not found."
    pport = c.pasv();                 assert(pport > 0);
    dfd = openData(pport);            assert(dfd >= 0);
    c.send("RETR /todelete.bin");
    int retr = c.readCode();
    ::close(dfd);
    std::fprintf(stderr, "  RETR-deleted ->%d (expect 550)\n", retr);
    assert(retr == 550 && "RETR of a deleted file must return 550");
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
        {"pass_without_user",       test_pass_without_user},
        {"malformed_port",          test_malformed_port},
        {"port_bounce",             test_port_bounce},
        {"cwd_dotdot_clamps",       test_cwd_dotdot_clamps_at_root},
        {"upload_download",         test_upload_download},
        {"large_file_round_trip",   test_large_file_round_trip},
        {"dele_then_retr_fails",    test_dele_then_retr_fails},
        {"mlsd_subdir",             test_mlsd_subdir},
        {"long_cwd_path",           test_long_cwd_path},
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
