#!/usr/bin/env python3
"""
setup_dlss.py — stage NVIDIA Streamline/DLSS binaries for Pyxis (optional).

DLSS Stage 1 (rtx-realtime-alignment-design.md, "DLSS — corrected stance"):
an Apache-2.0 app MAY use DLSS as an optional proprietary runtime
integration, as long as the repo itself stays clean of NVIDIA's proprietary
SDK materials (NVIDIA RTX SDKs License §4(e) forbids OSS-license
contamination). The compliant shape:

  - Streamline's SOURCE (interposer, headers, plugin manager) is
    MIT-licensed — safe to build from source, but Pyxis doesn't vendor it
    either; there is nothing to build here in Stage 1.
  - The prebuilt NGX / DLSS plugin binaries (sl.interposer.dll,
    nvngx_dlss*.dll, ...) are proprietary. This script downloads them into
    an UNTRACKED directory (default `_local/dlss/`, already covered by
    .gitignore's `_local/` entry) — never into a tracked path.
  - Pyxis's DlssProvider (Private/Dlss/DlssProvider.h) finds them at
    runtime via the PYXIS_DLSS_PATH environment variable, or by falling
    back to <exe-dir> if you copy them next to pyxis.exe yourself.

This script never runs automatically (no CMake / CI hook calls it) and
never accepts NVIDIA's license on your behalf — you must pass
--accept-nvidia-license explicitly after reading the notice it prints.

Usage:
    python _tools/setup_dlss.py --accept-nvidia-license
    python _tools/setup_dlss.py --accept-nvidia-license --version 2.7.1
    python _tools/setup_dlss.py --accept-nvidia-license --url <direct-zip-url>
    python _tools/setup_dlss.py --accept-nvidia-license --dest D:/sdks/dlss

If the download fails (offline machine, NVIDIA changed the release layout,
sandboxed CI, ...) the script prints manual-download + copy instructions
and exits non-zero rather than leaving a half-unpacked directory silently.
"""

from __future__ import annotations

import argparse
import shutil
import sys
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DEST = REPO_ROOT / "_local" / "dlss"

STREAMLINE_REPO_URL = "https://github.com/NVIDIAGameWorks/Streamline"
STREAMLINE_RELEASES_URL = f"{STREAMLINE_REPO_URL}/releases"

NVIDIA_LICENSE_NOTICE = f"""
NOTICE — NVIDIA RTX SDKs License (read before continuing)
-----------------------------------------------------------
Streamline's SOURCE (interposer, headers, plugin manager) is MIT-licensed.
The prebuilt NGX / DLSS plugin binaries this script downloads
(sl.interposer.dll, nvngx_dlss*.dll, and friends) are distributed under
NVIDIA's proprietary RTX SDKs License, NOT Apache-2.0 or MIT.

Per that license's §4(e) ("no OSS-license contamination"), Pyxis
(Apache-2.0) NEVER vendors or commits these binaries into the repo. This
script downloads them into an UNTRACKED directory only ({DEFAULT_DEST},
already excluded via .gitignore's `_local/` entry); Pyxis loads them at
runtime, outside the repo, via PYXIS_DLSS_PATH or by finding them next to
pyxis.exe.

By passing --accept-nvidia-license you confirm you have read and accept
NVIDIA's license terms for the downloaded binaries:
    {STREAMLINE_REPO_URL}/blob/main/LICENSE.txt
    {STREAMLINE_REPO_URL}/blob/main/LICENSE-NVIDIA.txt

See _documentation/rtx-realtime-alignment-design.md, "DLSS — corrected
stance" for the full policy this script implements (Stage 1 = this
scaffold; Stage 2 = the actual Streamline slInit hookup + two-resolution
pipeline).
"""


def build_zip_url(version: str) -> str:
    # Best-effort guess at Streamline's release-asset naming convention;
    # NVIDIA has changed this layout across releases before, so --url lets
    # you override it entirely once you've looked up the real asset name at
    # STREAMLINE_RELEASES_URL. A wrong guess here fails the download step
    # cleanly (see main()'s except branch) rather than corrupting anything.
    return f"{STREAMLINE_REPO_URL}/releases/download/v{version}/streamline-sdk-v{version}.zip"


def print_copy_instructions(dest: Path) -> None:
    print(
        f"""
Done. {dest} now holds the unpacked Streamline SDK contents (untracked —
see .gitignore's `_local/` entry; `git status` should show nothing new).

Next step — Pyxis's DLSS Stage 1 capability probe (DlssProvider) looks for
sl.interposer.dll at PYXIS_DLSS_PATH, or next to pyxis.exe:

  1. Find sl.interposer.dll and nvngx_dlss*.dll somewhere under {dest}
     (typically a bin/x64/ subdirectory in Streamline's own release
     layout — the exact path varies by version).
  2. EITHER copy both next to pyxis.exe (<build>/<preset>/bin/<Config>/),
     OR point Pyxis at the directory containing them without copying:
         setx PYXIS_DLSS_PATH "<directory containing sl.interposer.dll>"
     (setx persists across sessions but needs a fresh shell; for the
     current shell only: `set PYXIS_DLSS_PATH=...` / PowerShell
     `$env:PYXIS_DLSS_PATH = "..."`).
  3. Run pyxis.exe (viewer or --headless) and check the log for a line
     like:
         denoiser: requested=Dlss effective=Builtin (reason: ...)
     `effective=Builtin` with a reason means the probe found a problem
     (missing DLL, wrong version, ...) — the reason names it. Note that
     `effective=Dlss` is UNREACHABLE in Stage 1 even once the DLLs are
     staged and resolve cleanly (see Private/Dlss/DlssProvider.h): the
     actual slInit call + two-resolution render pipeline is Stage 2 work,
     not yet implemented.
"""
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--accept-nvidia-license",
        action="store_true",
        help="Required. Confirms you have read and accept NVIDIA's RTX SDKs "
        "License for the downloaded binaries (see the printed notice).",
    )
    parser.add_argument(
        "--version",
        default="2.7.1",
        help="Streamline release tag to fetch (default: %(default)s). Browse "
        f"available releases at {STREAMLINE_RELEASES_URL}",
    )
    parser.add_argument(
        "--url",
        default=None,
        help="Override the release zip URL entirely (bypasses --version's guessed "
        "naming convention). Use this if the default fails.",
    )
    parser.add_argument(
        "--dest",
        default=str(DEFAULT_DEST),
        help=f"Untracked destination directory (default: {DEFAULT_DEST}).",
    )
    args = parser.parse_args()

    print(NVIDIA_LICENSE_NOTICE)
    print(f"Streamline source + releases: {STREAMLINE_RELEASES_URL}\n")

    if not args.accept_nvidia_license:
        print(
            "Refusing to download without --accept-nvidia-license. Re-run with "
            "that flag once you have read the notice above.",
            file=sys.stderr,
        )
        return 2

    url = args.url or build_zip_url(args.version)
    dest = Path(args.dest)
    dest.mkdir(parents=True, exist_ok=True)
    zip_path = dest / "streamline_download.zip"

    print(f"Downloading {url}")
    try:
        with urllib.request.urlopen(url, timeout=60) as response, open(zip_path, "wb") as out:
            shutil.copyfileobj(response, out)
    except (urllib.error.URLError, OSError, ValueError) as exc:
        print(
            f"\nDownload failed: {exc}\n"
            "This is expected on an offline / sandboxed machine, or if NVIDIA "
            f"changed the release asset layout since --version {args.version} was "
            f"guessed here. Download the SDK manually from:\n"
            f"    {STREAMLINE_RELEASES_URL}\n"
            f"then unzip it into {dest} yourself and follow the copy instructions "
            "below.",
            file=sys.stderr,
        )
        print_copy_instructions(dest)
        return 1

    print(f"Unpacking into {dest}")
    try:
        with zipfile.ZipFile(zip_path) as archive:
            archive.extractall(dest)
    finally:
        zip_path.unlink(missing_ok=True)

    print_copy_instructions(dest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
