# Changelog

Version format: `YYYY.MINOR.PATCH`

## [2026.0.0] - 2026-04-05

### Added
- Dual ESP32-C3 miner/mule architecture with UART JSON protocol
- Miner: connects to ezShare WiFi, downloads files, sends to mule via UART
- Mule: receives files via UART, caches in memory, serves via HTTP
- Captive portal on mule with DNS hijack for WiFi setup
- Single setup form collects home WiFi and ezShare credentials
- Mule sends ezShare creds to miner via UART on save, both reboot
- NVS credential persistence on both devices with Kconfig fallback
- ezShare-compatible HTTP API (/dir, /download, /api/status)
- Custom partition table (2MB app partition on 4MB flash)
