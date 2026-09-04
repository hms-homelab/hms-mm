#!/usr/bin/env python3
"""Boot a merged flash image in QEMU and prove the firmware actually starts.

    qemu_boot_test.py mule mule/build/merged.bin

Checking offsets inside a merged image says the bytes are in the right places.
It does not say the thing boots, and that is the failure worth catching: the
v1.0.0 images had the app at the wrong offset, flashed cleanly, and left a dead
board. So this runs the real image on an emulated ESP32-C3 and waits for the
firmware's own log lines.

WHY THE eFUSE FILE MATTERS
--------------------------
Booting a stock image under QEMU with no eFuse blob hangs before `app_main`,
with the last line being

    W (46x) eFuse: calibration efuse version does not match, set default version to 0

That warning is the cause, not a symptom. `esp_efuse_rtc_calib_get_ver()` returns
0 unless BLK_VERSION_MAJOR is 1, and with no stored calibration
`adc2_init_code_calibration()` — a global constructor, so it runs before
`main_task` — falls back to `adc_hal_self_calibration()`, which polls
`APB_SARADC.int_raw` for a conversion-done flag that QEMU's SAR ADC never
raises. It spins there forever. Confirmed by attaching gdb to the stalled
machine: the backtrace was read_cal_channel <- adc_hal_self_calibration <-
adc2_init_code_calibration <- do_global_ctors.

IDF's own default eFuse blob does not set that bit, so `idf.py qemu` hits this
too. Setting BLK_VERSION_MAJOR=1 sends the firmware down the read-from-eFuse
path instead, it never self-calibrates, and boot proceeds normally. Nothing
about the firmware changes; this only stands in for calibration data that real
silicon ships with.
"""

import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# ESP32-C3 eFuse layout, from components/efuse/esp32c3/esp_efuse_table.csv.
# BLK0 is 24 bytes, so BLK1 starts at 24 and BLK2 at 48. Cross-check: IDF's own
# blob has one non-zero byte, 0x0c at 38, which is WAFER_VERSION_MINOR_LO (BLK1
# bit 114, value 3) — 24 + 114//8 == 38, and 3 << (114 % 8) == 0x0c.
EFUSE_SIZE = 1024
BLK1_START = 24
BLK2_START = 48
WAFER_VERSION_MINOR_LO_BIT = 114   # 3 => chip revision v0.3
BLK_VERSION_MAJOR_BIT = 128        # in BLK2; 1 => "With calibration"

BOOT_SECONDS = 45

# Lines every board must produce. The offset is spelled out because that is the
# exact thing v1.0.0 got wrong.
REQUIRED = [
    "Loaded app from partition at offset 0x20000",
    "main_task: Calling app_main()",
]

# Per-board proof that this board's own app_main ran, not just some app.
BOARD_MARKERS = {
    "mule": ["hms-mm mule", "USB provisioning ready"],
    "miner": ["hms-mm miner"],
}

# A crash looks like a successful run right up until the markers are missing, so
# name the failure instead of reporting "marker not found".
CRASH_PATTERNS = [
    "Guru Meditation",
    "rst:0x3 (RTC_SW_SYS_RST)",
    "No bootable app partitions",
    "invalid magic byte",
    "abort() was called",
]


def write_efuse(path: Path) -> None:
    blob = bytearray(EFUSE_SIZE)
    blob[BLK1_START + WAFER_VERSION_MINOR_LO_BIT // 8] |= 3 << (WAFER_VERSION_MINOR_LO_BIT % 8)
    blob[BLK2_START + BLK_VERSION_MAJOR_BIT // 8] |= 1 << (BLK_VERSION_MAJOR_BIT % 8)
    path.write_bytes(bytes(blob))


def find_qemu() -> str:
    """On PATH once export.sh has run; otherwise search the IDF tools dirs."""
    from shutil import which
    found = which("qemu-system-riscv32")
    if found:
        return found

    # espressif/idf containers keep tools under /opt/esp/tools, a local install
    # under ~/.espressif/tools, and IDF_TOOLS_PATH overrides both.
    roots = [Path(os.environ["IDF_TOOLS_PATH"])] if os.environ.get("IDF_TOOLS_PATH") else []
    roots += [Path("/opt/esp"), Path.home() / ".espressif"]
    for root in roots:
        candidates = sorted(root.glob("tools/qemu-riscv32/*/qemu/bin/qemu-system-riscv32"))
        if candidates:
            return str(candidates[-1])

    sys.exit("::error::qemu-system-riscv32 not found. "
             "Install with: python3 $IDF_PATH/tools/idf_tools.py install qemu-riscv32")


def run(board: str, image: Path, workdir: Path) -> str:
    # QEMU expects a full-size flash device, and a merged image stops after the
    # last part.
    flash = workdir / "flash.bin"
    flash.write_bytes(image.read_bytes().ljust(4 * 1024 * 1024, b"\xff"))

    efuse = workdir / "efuse.bin"
    write_efuse(efuse)

    log = workdir / "boot.log"
    log.touch()

    # QEMU never exits: the firmware runs forever, as it should. So it is
    # started, watched until every marker it is going to produce has appeared,
    # and then killed. Polling the log means a healthy board finishes in a
    # couple of seconds instead of always paying the full timeout.
    wanted = REQUIRED + BOARD_MARKERS[board]
    proc = subprocess.Popen(
        [
            find_qemu(), "-nographic", "-M", "esp32c3",
            "-drive", f"file={flash},if=mtd,format=raw",
            "-drive", f"file={efuse},if=none,format=raw,id=efuse",
            "-global", "driver=nvram.esp32c3.efuse,property=drive,value=efuse",
            # Without this the emulated watchdog fires during the long waits
            # this firmware does when the other board is absent.
            "-global", "driver=timer.esp32c3.timg,property=wdt_disable,value=true",
            "-nic", "user,model=open_eth",
            "-serial", f"file:{log}",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    try:
        deadline = time.monotonic() + BOOT_SECONDS
        while time.monotonic() < deadline:
            text = log.read_text(errors="replace")
            if all(m in text for m in wanted) or any(c in text for c in CRASH_PATTERNS):
                break
            if proc.poll() is not None:      # QEMU died on its own
                break
            time.sleep(0.5)
    finally:
        proc.kill()
        proc.wait()

    stderr = proc.stderr.read().decode(errors="replace") if proc.stderr else ""
    if stderr.strip():
        print(f"note: qemu stderr: {stderr.strip()[:400]}")

    return re.sub(r"\x1b\[[0-9;]*m", "", log.read_text(errors="replace"))


def main(argv: list[str]) -> int:
    if len(argv) != 3 or argv[1] not in BOARD_MARKERS:
        print(f"usage: {argv[0]} <{'|'.join(BOARD_MARKERS)}> <merged.bin>", file=sys.stderr)
        return 2

    board, image = argv[1], Path(argv[2])
    if not image.is_file():
        print(f"::error::{image} does not exist")
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        output = run(board, image, Path(tmp))

    failed = False
    for pattern in CRASH_PATTERNS:
        if pattern in output:
            print(f"::error::{board} crashed or failed to boot: saw {pattern!r}")
            failed = True

    for marker in REQUIRED + BOARD_MARKERS[board]:
        if marker in output:
            print(f"ok  {board}: {marker!r}")
        else:
            print(f"::error::{board}: never printed {marker!r}")
            failed = True

    if failed:
        print(f"--- {board} boot log ---")
        print(output[-3000:])
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
