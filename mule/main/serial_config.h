#pragma once

// Provisioning over the mule's USB port.
//
// The web flasher at https://hms-homelab.github.io/hms-mm/ collects the home
// WiFi and ezShare credentials in the browser and writes them down the same
// USB cable it just flashed through, so nobody has to join a setup AP. The
// captive portal still exists and is still the fallback; this is an extra
// route to the same NVS keys.
//
// Starts a task, so it must be running before app_main blocks or returns.
void serial_config_start(void);
