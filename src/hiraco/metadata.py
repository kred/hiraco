from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any


class MetadataToolError(RuntimeError):
    pass


@dataclass(frozen=True)
class RawpySummary:
    path: str
    raw_type: str | None
    width: int | None
    height: int | None
    visible_width: int | None
    visible_height: int | None
    color_desc: str | None
    num_colors: int | None
    black_level_per_channel: list[int] | None
    camera_white_level_per_channel: list[int] | None
    white_level: int | None
    camera_whitebalance: list[float] | None
    daylight_whitebalance: list[float] | None
    raw_pattern: list[list[int]] | None


def _run_exiftool_json(path: Path) -> dict[str, Any]:
    try:
        completed = subprocess.run(
            ["exiftool", "-j", "-G1", "-a", "-u", "-n", str(path)],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as exc:
        raise MetadataToolError(f"failed to execute exiftool: {exc}") from exc

    if completed.returncode != 0:
        raise MetadataToolError(completed.stderr.strip() or "exiftool failed")

    payload = json.loads(completed.stdout)
    if not payload:
        return {}
    return payload[0]


def _safe_list(value: Any) -> list[Any] | None:
    if value is None:
        return None
    try:
        return list(value)
    except TypeError:
        return None


def _safe_matrix(value: Any) -> list[list[int]] | None:
    if value is None:
        return None
    try:
        return [list(row) for row in value.tolist()]
    except AttributeError:
        try:
            return [list(row) for row in value]
        except TypeError:
            return None


def inspect_with_rawpy(path: Path) -> RawpySummary:
    import rawpy

    with rawpy.imread(str(path)) as raw:
        return RawpySummary(
            path=str(path),
            raw_type=str(raw.raw_type),
            width=getattr(raw.sizes, "width", None),
            height=getattr(raw.sizes, "height", None),
            visible_width=getattr(raw.sizes, "raw_width", None),
            visible_height=getattr(raw.sizes, "raw_height", None),
            color_desc=raw.color_desc.decode("ascii", errors="ignore") if raw.color_desc is not None else None,
            num_colors=raw.num_colors,
            black_level_per_channel=_safe_list(raw.black_level_per_channel),
            camera_white_level_per_channel=_safe_list(raw.camera_white_level_per_channel),
            white_level=raw.white_level,
            camera_whitebalance=_safe_list(raw.camera_whitebalance),
            daylight_whitebalance=_safe_list(raw.daylight_whitebalance),
            raw_pattern=_safe_matrix(raw.raw_pattern),
        )


def inspect_file(path: Path) -> dict[str, Any]:
    result: dict[str, Any] = {
        "path": str(path),
        "exists": path.exists(),
        "suffix": path.suffix.lower(),
    }

    if path.exists():
        stat = path.stat()
        result["size_bytes"] = stat.st_size

    rawpy_error = None
    try:
        result["rawpy"] = inspect_with_rawpy(path).__dict__
    except Exception as exc:  # rawpy raises library-specific exceptions
        rawpy_error = str(exc)
        result["rawpy"] = None

    exiftool_error = None
    try:
        result["exiftool"] = _run_exiftool_json(path)
    except Exception as exc:
        exiftool_error = str(exc)
        result["exiftool"] = None

    if rawpy_error is not None:
        result["rawpy_error"] = rawpy_error
    if exiftool_error is not None:
        result["exiftool_error"] = exiftool_error

    return result


def compact_source_summary(path: Path) -> dict[str, Any]:
    inspection = inspect_file(path)
    exiftool_data = inspection.get("exiftool") or {}
    rawpy_data = inspection.get("rawpy") or {}
    return {
        "path": inspection["path"],
        "size_bytes": inspection.get("size_bytes"),
        "make": exiftool_data.get("EXIF:Make") or exiftool_data.get("IFD0:Make"),
        "model": exiftool_data.get("EXIF:Model") or exiftool_data.get("IFD0:Model"),
        "raw_type": rawpy_data.get("raw_type"),
        "width": rawpy_data.get("width"),
        "height": rawpy_data.get("height"),
        "num_colors": rawpy_data.get("num_colors"),
    }


def metadata_diff(source: Path, destination: Path) -> dict[str, Any]:
    source_data = _run_exiftool_json(source)
    destination_data = _run_exiftool_json(destination)

    source_keys = set(source_data)
    destination_keys = set(destination_data)

    changed = {}
    for key in sorted(source_keys & destination_keys):
        if source_data[key] != destination_data[key]:
            changed[key] = {
                "source": source_data[key],
                "destination": destination_data[key],
            }

    return {
        "source": str(source),
        "destination": str(destination),
        "source_tag_count": len(source_data),
        "destination_tag_count": len(destination_data),
        "missing_in_destination": {key: source_data[key] for key in sorted(source_keys - destination_keys)},
        "added_in_destination": {key: destination_data[key] for key in sorted(destination_keys - source_keys)},
        "changed": changed,
    }