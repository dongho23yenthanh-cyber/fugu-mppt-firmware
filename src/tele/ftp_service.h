#pragma once

// FtpService: exposes the littlefs /conf partition over FTP while Wi-Fi is up. Owns the FtpServer
// object, its callbacks and lifecycle (implemented in ftp_service.cpp); no dependency on the
// main.cpp globals.

#include <SimpleFTPServer.h>

#include "../service.h"

class FtpService : public Service {
public:
    FtpService() : Service("ftp", "/littlefs/conf/ftp.conf", /*requiresNetwork*/ true) {
    }

    std::string statusDetail() const override { return ""; }

    // Set from the SimpleFTPServer connect/disconnect callback (ftp_service.cpp), which is a plain
    // function pointer and so reaches this through the global `ftpService` instance.
    //volatile bool _clientConnected = false;

protected:
    bool onStart() override;

    void onStop() override;

    void onTick() override;

private:
    // SimpleFTPServer's begin() stores the user/pass *pointers* (const char*), not copies, so the
    // backing strings must outlive ftpSrv — keep them here, not as onStart() locals.
    std::string _user, _pass;
    FtpServer ftpSrv;
};

inline FtpService ftpService;
