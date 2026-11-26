#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

struct CloudToolCredentials {
    const char* protocol;
    const char* host;
    const char* path;
    const char* client_id;
    const char* username;
    const char* password;
    const char* down;
    const char* up;
    int port;
    uint32_t expires_in;
};

esp_err_t cloud_tools_start(const CloudToolCredentials& credentials, const char* session_id);
bool cloud_tools_topics_match(const char* down, const char* up);
void cloud_tools_enable();
