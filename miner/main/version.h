#pragma once

/*
 * Miner firmware version — this board only.
 *
 * The mule and the miner version INDEPENDENTLY. A change often touches one
 * board and not the other, and once OTA lands either board can be updated
 * without the other, so in the field they legitimately differ. Nothing in the
 * firmware treats a version difference as a fault; the mule reports this string
 * as `miner_fw` in /api/status so a pair can be identified.
 *
 * The release workflow derives each artefact's filename from that board's own
 * version, and requires the pushed tag to match at least one of them.
 */

#define VERSION_MAJOR   1
#define VERSION_MINOR   0
#define VERSION_PATCH   0

#define _STRINGIFY(x) #x
#define STRINGIFY(x) _STRINGIFY(x)

#define FIRMWARE_VERSION \
    STRINGIFY(VERSION_MAJOR) "." STRINGIFY(VERSION_MINOR) "." STRINGIFY(VERSION_PATCH)
