#pragma once

/*
 * Mule firmware version — this board only.
 *
 * The mule and the miner version INDEPENDENTLY. A change often touches one
 * board and not the other, and once OTA lands either board can be updated
 * without the other, so in the field they legitimately differ. Nothing in the
 * firmware treats a version difference as a fault; /api/status simply reports
 * both (`fw` from here, `miner_fw` read back over the link) so a pair can be
 * identified.
 *
 * The release workflow derives each artefact's filename from that board's own
 * version, and requires the pushed tag to match at least one of them.
 */

#define VERSION_YEAR    2026
#define VERSION_MINOR   1
#define VERSION_PATCH   1

#define _STRINGIFY(x) #x
#define STRINGIFY(x) _STRINGIFY(x)

#define FIRMWARE_VERSION \
    STRINGIFY(VERSION_YEAR) "." STRINGIFY(VERSION_MINOR) "." STRINGIFY(VERSION_PATCH)
