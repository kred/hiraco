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
    color_matrix: list[list[float]] | None
    crop_left_margin: int | None
    crop_top_margin: int | None
    crop_width: int | None
    crop_height: int | None


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


def _first_channel(value: list[Any] | None) -> int | None:
    if not value:
        return None
    try:
        return int(value[0])
    except (TypeError, ValueError):
        return None


def _scale_level_to_linear_dng(level: int | None, white_level: int | None) -> int | None:
    if level is None or white_level is None or white_level <= 0:
        return None
    shift = max(0, 16 - int(white_level).bit_length())
    return int(level) << shift


def _compute_as_shot_neutral(camera_whitebalance: list[float] | None) -> list[float] | None:
    if not camera_whitebalance or len(camera_whitebalance) < 3:
        return None

    neutral: list[float] = []
    for index in range(3):
        gain = float(camera_whitebalance[index])
        if gain <= 0:
            return None
        neutral.append(1.0 / gain)

    green = neutral[1]
    if green <= 0:
        return None
    return [value / green for value in neutral]


def inspect_with_rawpy(path: Path) -> RawpySummary:
    import rawpy

    with rawpy.imread(str(path)) as raw:
        sizes = raw.sizes
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
            color_matrix=_safe_matrix(raw.color_matrix),
            crop_left_margin=getattr(sizes, "crop_left_margin", None),
            crop_top_margin=getattr(sizes, "crop_top_margin", None),
            crop_width=getattr(sizes, "crop_width", None),
            crop_height=getattr(sizes, "crop_height", None),
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
    black_level = _first_channel(rawpy_data.get("black_level_per_channel"))
    white_level = rawpy_data.get("white_level")
    make = exiftool_data.get("EXIF:Make") or exiftool_data.get("IFD0:Make")
    model = exiftool_data.get("EXIF:Model") or exiftool_data.get("IFD0:Model")
    unique_camera_model = None
    if make and model:
        unique_camera_model = f"{make} {model}"

    flattened_color_matrix: dict[str, float] = {}
    color_matrix = rawpy_data.get("color_matrix") or []
    if len(color_matrix) >= 3:
        for row in range(3):
            if len(color_matrix[row]) >= 3:
                for column in range(3):
                    key = f"linear_dng_color_matrix_{row}{column}"
                    flattened_color_matrix[key] = float(color_matrix[row][column])

    as_shot_neutral = _compute_as_shot_neutral(rawpy_data.get("camera_whitebalance"))
    return {
        "path": inspection["path"],
        "size_bytes": inspection.get("size_bytes"),
        "make": make,
        "model": model,
        "unique_camera_model": unique_camera_model,
        "raw_type": rawpy_data.get("raw_type"),
        "width": rawpy_data.get("width"),
        "height": rawpy_data.get("height"),
        "num_colors": rawpy_data.get("num_colors"),
        "linear_dng_black_level": _scale_level_to_linear_dng(black_level, white_level),
        "linear_dng_white_level": 65535 if white_level is not None else None,
        "linear_dng_crop_left_margin": rawpy_data.get("crop_left_margin"),
        "linear_dng_crop_top_margin": rawpy_data.get("crop_top_margin"),
        "linear_dng_crop_width": rawpy_data.get("crop_width"),
        "linear_dng_crop_height": rawpy_data.get("crop_height"),
        "linear_dng_as_shot_neutral_0": None if as_shot_neutral is None else as_shot_neutral[0],
        "linear_dng_as_shot_neutral_1": None if as_shot_neutral is None else as_shot_neutral[1],
        "linear_dng_as_shot_neutral_2": None if as_shot_neutral is None else as_shot_neutral[2],
        **flattened_color_matrix,
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