#include "logging.h"

#include <ESPmDNS.h>

void webserver_begin(void) {
    MDNS.addService("http", "tcp", 80);
}
