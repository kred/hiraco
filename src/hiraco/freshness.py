from __future__ import annotations

import importlib
import json
import re
import subprocess
from dataclasses import dataclass
from html import unescape
import shutil
from pathlib import Path
from typing import Iterable
from urllib.error import URLError
from urllib.request import urlopen


RAWPY_URL = "https://pypi.org/pypi/rawpy/json"
EXIFTOOL_URL = "https://exiftool.org/ver.txt"
LIBRAW_URL = "https://www.libraw.org/download"
ADOBE_DNG_URL = "https://helpx.adobe.com/camera-raw/digital-negative.html"


@dataclass(frozen=True)
class VersionStatus:
    name: str
    local: str | None
    upstream: str | None
    details: str


def _fetch_text(url: str, timeout: float = 10.0) -> str:
    with urlopen(url, timeout=timeout) as response:
        return response.read().decode("utf-8", errors="replace")


def _import_version(module_name: str, attribute: str = "__version__") -> str | None:
    try:
        module = importlib.import_module(module_name)
    except ImportError:
        return None
    return getattr(module, attribute, None)


def _command_output(command: Iterable[str]) -> str | None:
    try:
        completed = subprocess.run(
            list(command),
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout.strip() or None


def get_local_status(workspace_root: Path) -> list[VersionStatus]:
    rawpy_version = _import_version("rawpy")
    exiftool_version = _command_output(["exiftool", "-ver"])
    cmake_version = _command_output(["cmake", "--version"])
    dng_sdk_root = workspace_root / "dng_sdk_1_7_1"
    dng_sdk_version = "1.7.1 (vendored folder)" if dng_sdk_root.exists() else None
    system_libraw = None
    if shutil.which("brew") is not None:
        system_libraw = _command_output(["brew", "list", "--versions", "libraw"])
    libraw_version = None
    if rawpy_version is not None:
        try:
            rawpy = importlib.import_module("rawpy")
            libraw_version = getattr(rawpy, "libraw_version", None)
        except ImportError:
            libraw_version = None
    if isinstance(libraw_version, tuple):
        libraw_version = ".".join(str(part) for part in libraw_version)

    return [
        VersionStatus("rawpy", rawpy_version, None, "Python raw decoder wrapper"),
        VersionStatus("LibRaw", libraw_version, None, "Usually discovered through rawpy"),
        VersionStatus("system LibRaw", system_libraw, None, "Native helper dependency"),
        VersionStatus("ExifTool", exiftool_version, None, "Metadata extraction and copy tool"),
        VersionStatus("CMake", cmake_version.splitlines()[0] if cmake_version else None, None, "Native build tool"),
        VersionStatus("Adobe DNG SDK", dng_sdk_version, None, "Vendored SDK folder presence"),
    ]


def get_upstream_status() -> list[VersionStatus]:
    statuses: list[VersionStatus] = []

    rawpy_data = json.loads(_fetch_text(RAWPY_URL))
    statuses.append(
        VersionStatus(
            name="rawpy",
            local=None,
            upstream=str(rawpy_data["info"]["version"]),
            details="Latest PyPI release",
        )
    )

    statuses.append(
        VersionStatus(
            name="ExifTool",
            local=None,
            upstream=_fetch_text(EXIFTOOL_URL).strip(),
            details="Latest published version",
        )
    )

    libraw_html = unescape(_fetch_text(LIBRAW_URL))
    libraw_match = re.search(r"Release version:\s*LibRaw\s+([0-9][0-9A-Za-z.\-]+)", libraw_html)
    statuses.append(
        VersionStatus(
            name="LibRaw",
            local=None,
            upstream=libraw_match.group(1) if libraw_match else None,
            details="Latest stable release page",
        )
    )

    adobe_html = unescape(_fetch_text(ADOBE_DNG_URL))
    adobe_match = re.search(
        r"Download the Adobe DNG SDK.*?\(([^)]+)\)",
        adobe_html,
        re.DOTALL,
    )
    statuses.append(
        VersionStatus(
            name="Adobe DNG SDK",
            local=None,
            upstream=adobe_match.group(1).strip() if adobe_match else None,
            details="Adobe public DNG page",
        )
    )

    return statuses


def safe_get_upstream_status() -> tuple[list[VersionStatus], list[str]]:
    try:
        return get_upstream_status(), []
    except (URLError, TimeoutError, OSError, json.JSONDecodeError) as exc:
        return [], [f"Upstream check failed: {exc}"]
