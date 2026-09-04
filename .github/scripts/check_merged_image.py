#!/usr/bin/env python3
"""Prove a merged flash image carries each part at the offset flash_args names.

    check_merged_image.py mule/build

A merged image built from stale offsets flashes without complaint and then does
nothing, which is exactly the failure v1.0.0 shipped: the app was merged at
0x10000, a leftover from the single-`factory` layout, while the dual-OTA table
puts it at 0x20000. Nothing downstream notices — not esptool, not the browser
flasher, not the board, which simply sits there.

So this reads `flash_args` (what `idf.py flash` itself would use) and checks the
merged image byte for byte at every offset in it. The one part that is allowed
to differ is the bootloader at 0x0: `merge_bin` rewrites its flash mode, size
and frequency into the header and updates the image SHA, so its first bytes and
its last 32 are legitimately not what the source file holds.
"""

import sys
from pathlib import Path

# esp_image_header_t is 24 bytes; merge_bin rewrites flash size/freq/mode in
# byte 3 and the SPI pin config, then re-hashes. Skipping the header and the
# trailing digest still compares the whole body.
HEADER_BYTES = 24
SHA_BYTES = 32


def parse_flash_args(build_dir: Path) -> list[tuple[int, Path]]:
    """[(offset, file)] from the build's own flash_args, ignoring the flags line."""
    parts = []
    for line in (build_dir / "flash_args").read_text().splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0].startswith("0x"):
            parts.append((int(fields[0], 16), build_dir / fields[1]))
    return parts


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} <build-dir>", file=sys.stderr)
        return 2

    build_dir = Path(argv[1])
    merged = (build_dir / "merged.bin").read_bytes()
    parts = parse_flash_args(build_dir)

    if not parts:
        print(f"::error::no parts parsed from {build_dir}/flash_args")
        return 1

    failed = False
    for offset, path in sorted(parts):
        want = path.read_bytes()
        got = merged[offset:offset + len(want)]

        if len(got) < len(want):
            print(
                f"::error::{path.name} needs {len(want)} bytes at {offset:#x} but "
                f"the merged image is only {len(merged)} bytes"
            )
            failed = True
            continue

        # The bootloader is rewritten in place by merge_bin; every other part is
        # copied verbatim.
        if offset == 0:
            want, got = want[HEADER_BYTES:-SHA_BYTES], got[HEADER_BYTES:-SHA_BYTES]

        if want == got:
            print(f"ok  {offset:#08x}  {path.name} ({len(want)} bytes verified)")
        else:
            first = next(i for i, (a, b) in enumerate(zip(want, got)) if a != b)
            print(
                f"::error::{path.name} is not at {offset:#x} in the merged image "
                f"(first difference at byte {first}: "
                f"{want[first]:#04x} != {got[first]:#04x})"
            )
            failed = True

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
