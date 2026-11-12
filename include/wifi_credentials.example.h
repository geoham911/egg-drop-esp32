#pragma once

#include <stddef.h>

struct WifiCredential {
  const char* ssid;
  const char* password;
};

// Replace the empty strings with your network names and WPA2 passwords.
const WifiCredential WIFI_NETWORKS[] = {
    {"", ""},
    {"", ""},
};
constexpr size_t WIFI_NETWORK_COUNT =
    sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);
