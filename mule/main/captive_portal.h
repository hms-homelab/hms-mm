#pragma once
// Start captive portal AP mode with DNS hijack.
// Form collects home WiFi + ezShare credentials.
// Stores WiFi in NVS, sends ezShare creds to miner via UART, reboots.
void captive_portal_start(void);
