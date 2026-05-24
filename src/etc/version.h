#pragma once

#include <esp_app_desc.h>
#include <cstdio>

#define FIRMWARE_VERSION "2.1.0"

inline const char *format_version() {
    static char buf[160];
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(buf, sizeof(buf), "%s v" FIRMWARE_VERSION " %s (built %s %s, IDF %s)",
             app->project_name, app->version, app->date, app->time, app->idf_ver);
    return buf;
}
