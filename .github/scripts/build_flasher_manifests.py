#!/usr/bin/env python3
"""Write the ESP Web Tools manifests for the web flasher, then check them.

Called by .github/workflows/pages.yml against the directory holding the merged
images already downloaded from a release:

    build_flasher_manifests.py site/firmware

The two boards version independently, so the version is read off each file
(`mule-1.0.1-merged.bin`) rather than passed in: a release that bumps only the
mule still publishes both, carrying different numbers. The versions found are
printed as `name=value` lines for the workflow to substitute into the page.

A manifest that names a file which is not there, or that does not parse, fails
silently in the browser as a button that appears to do nothing. That is worth
catching in CI rather than in someone else's hands, so every manifest is read
back and its firmware confirmed present and plausibly sized.
"""

import json
import re
import sys
from pathlib import Path

# board -> label shown while flashing
#
# Both entries are full-flash images written at offset 0, so each manifest needs
# a single part and nothing here has to track partitions.csv. The offsets match
# the merge in release.yml (bootloader 0x0, table 0x8000, app 0x10000).
BOARDS = {
    "mule": "hms-mm mule (home WiFi side)",
    "miner": "hms-mm miner (SD card / O2Ring side)",
}

MERGED = re.compile(r"^(mule|miner)-(\d+\.\d+\.\d+)-merged\.bin$")

# A merged ESP32-C3 image is ~1MB. Anything far below that is a truncated
# download or an error page saved under a .bin name.
MIN_PLAUSIBLE_BYTES = 500_000


def find_images(firmware_dir: Path) -> dict[str, tuple[str, str]]:
    """board -> (filename, version), from whatever merged images are present."""
    found: dict[str, tuple[str, str]] = {}
    for path in sorted(firmware_dir.iterdir()):
        match = MERGED.match(path.name)
        if match:
            found[match.group(1)] = (path.name, match.group(2))
    return found


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} <firmware-dir>", file=sys.stderr)
        return 2

    firmware_dir = Path(argv[1])
    images = find_images(firmware_dir)

    missing = sorted(set(BOARDS) - set(images))
    if missing:
        print(
            "::error::no merged image for " + ", ".join(missing) +
            f" in {firmware_dir} (have: " +
            ", ".join(p.name for p in sorted(firmware_dir.iterdir())) + ")"
        )
        return 1

    for board, label in BOARDS.items():
        filename, version = images[board]
        manifest = {
            "name": label,
            "version": version,
            "new_install_prompt_erase": True,
            "builds": [
                {
                    "chipFamily": "ESP32-C3",
                    "parts": [{"path": filename, "offset": 0}],
                }
            ],
        }
        path = firmware_dir / f"manifest-{board}.json"
        path.write_text(json.dumps(manifest, indent=2) + "\n")
        # Consumed by pages.yml to fill the page's version placeholders.
        print(f"{board}_version={version}")

    return verify(firmware_dir)


def verify(firmware_dir: Path) -> int:
    """Read every manifest back the way the browser will, and prove it resolves."""
    failed = False

    manifests = sorted(firmware_dir.glob("manifest-*.json"))
    if len(manifests) != len(BOARDS):
        print(f"::error::wrote {len(BOARDS)} manifests but found {len(manifests)}")
        return 1

    for path in manifests:
        try:
            manifest = json.loads(path.read_text())
        except json.JSONDecodeError as exc:
            print(f"::error file={path}::invalid JSON: {exc}")
            failed = True
            continue

        for build in manifest["builds"]:
            for part in build["parts"]:
                binary = firmware_dir / part["path"]
                if not binary.is_file():
                    print(f"::error file={path}::missing firmware {part['path']}")
                    failed = True
                    continue
                size = binary.stat().st_size
                print(f"{path.name} -> {part['path']} ({size:,} bytes)")
                if size < MIN_PLAUSIBLE_BYTES:
                    print(
                        f"::error file={path}::{part['path']} is only {size} bytes, "
                        "which is too small to be a merged image"
                    )
                    failed = True

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
