from __future__ import annotations

import hashlib
import json
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any


class MetadataToolError(RuntimeError):
    pass


OLYMPUS_UNKNOWN_BLOCK_TAGS = (
    "UnknownBlock1",
    "UnknownBlock2",
    "UnknownBlock3",
    "UnknownBlock4",
)

OLYMPUS_CORRELATION_FIELDS = (
    ("Olympus:StackedImage", "stacked_image"),
    ("EXIF:LensModel", "lens_model"),
    ("Composite:LensID", "lens_id"),
    ("Composite:FocalLength35efl", "focal_length_35mm"),
    ("EXIF:FocalLength", "focal_length"),
    ("EXIF:ISO", "iso"),
    ("EXIF:ExposureTime", "exposure_time"),
    ("Olympus:CameraTemperature", "camera_temperature"),
    ("Olympus:DriveMode", "drive_mode"),
    ("Olympus:SpecialMode", "special_mode"),
)

OLYMPUS_CORRELATION_FIELD_NAMES = tuple(field_name for _, field_name in OLYMPUS_CORRELATION_FIELDS)


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


def _run_exiftool_json(path: Path, numeric: bool = True) -> dict[str, Any]:
    args = ["exiftool", "-j", "-G1", "-a", "-u"]
    if numeric:
        args.append("-n")
    args.append(str(path))
    try:
        completed = subprocess.run(
            args,
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


def _run_exiftool_binary(path: Path, tag: str) -> bytes:
    try:
        completed = subprocess.run(
            ["exiftool", "-b", "-u", f"-{tag}", str(path)],
            capture_output=True,
            check=False,
        )
    except OSError as exc:
        raise MetadataToolError(f"failed to execute exiftool: {exc}") from exc

    if completed.returncode != 0:
        message = completed.stderr.decode("utf-8", errors="replace").strip()
        raise MetadataToolError(message or f"exiftool failed for {tag}")

    return completed.stdout


def _decoded_stacked_image_label(path: Path, exiftool_data: dict[str, Any] | None = None) -> str | None:
    if exiftool_data is None:
        exiftool_data = _run_exiftool_json(path, numeric=False)
    value = exiftool_data.get("Olympus:StackedImage")
    return None if value in (None, "") else str(value)


def _normalized_partition(groups: list[list[str]]) -> tuple[tuple[str, ...], ...]:
    return tuple(sorted(tuple(sorted(group)) for group in groups))


def _extract_correlation_metadata(numeric_exiftool_data: dict[str, Any], decoded_exiftool_data: dict[str, Any]) -> dict[str, Any]:
    metadata: dict[str, Any] = {}
    for source_key, output_key in OLYMPUS_CORRELATION_FIELDS:
        source = decoded_exiftool_data if source_key == "Olympus:StackedImage" else numeric_exiftool_data
        value = source.get(source_key)
        if value is not None:
            metadata[output_key] = value
    return metadata


def _payload_diff_summary(payloads: list[bytes]) -> dict[str, Any]:
    if not payloads:
        return {
            "comparable": False,
            "reason": "no payloads",
        }

    payload_sizes = {len(payload) for payload in payloads}
    if len(payload_sizes) != 1:
        return {
            "comparable": False,
            "reason": "payload sizes differ",
            "payload_sizes": sorted(payload_sizes),
        }

    varying_offsets: list[int] = []
    payload_size = len(payloads[0])
    for offset in range(payload_size):
        first_value = payloads[0][offset]
        if any(payload[offset] != first_value for payload in payloads[1:]):
            varying_offsets.append(offset)

    ranges: list[dict[str, int]] = []
    if varying_offsets:
        start = varying_offsets[0]
        previous = start
        for offset in varying_offsets[1:]:
            if offset != previous + 1:
                ranges.append({"start": start, "end": previous, "length": previous - start + 1})
                start = offset
            previous = offset
        ranges.append({"start": start, "end": previous, "length": previous - start + 1})

    return {
        "comparable": True,
        "payload_size": payload_size,
        "varying_byte_count": len(varying_offsets),
        "varying_byte_ratio": 0.0 if payload_size == 0 else len(varying_offsets) / payload_size,
        "varying_range_count": len(ranges),
        "varying_ranges_preview": ranges[:32],
    }


def _matching_partition_fields(files: list[dict[str, Any]], payload_groups: list[dict[str, Any]]) -> list[str]:
    payload_partition = _normalized_partition([group["paths"] for group in payload_groups])

    matched_fields: list[str] = []
    for _, field_name in OLYMPUS_CORRELATION_FIELDS:
        grouped_values: dict[str, list[str]] = {}
        for file_record in files:
            value = file_record["capture_metadata"].get(field_name)
            if value is None:
                continue
            grouped_values.setdefault(str(value), []).append(file_record["path"])
        if not grouped_values:
            continue
        metadata_partition = _normalized_partition(list(grouped_values.values()))
        if metadata_partition == payload_partition:
            matched_fields.append(field_name)
    return matched_fields


def _build_block_summary(files: list[dict[str, Any]]) -> dict[str, Any]:
    block_summary: dict[str, Any] = {}
    for tag in OLYMPUS_UNKNOWN_BLOCK_TAGS:
        grouped: dict[str, dict[str, Any]] = {}
        payloads: list[bytes] = []
        for file_record in files:
            block = file_record["blocks"][tag]
            payload = block["payload"]
            payloads.append(payload)
            sha256 = block["sha256"]
            group = grouped.setdefault(
                sha256,
                {
                    "sha256": sha256,
                    "size_bytes": block["size_bytes"],
                    "paths": [],
                },
            )
            group["paths"].append(file_record["path"])

        block_summary[tag] = {
            "unique_payload_count": len(grouped),
            "groups": sorted(grouped.values(), key=lambda item: (-len(item["paths"]), item["sha256"])),
            "diff_summary": _payload_diff_summary(payloads),
        }

    for tag in OLYMPUS_UNKNOWN_BLOCK_TAGS:
        block_summary[tag]["matching_partition_fields"] = _matching_partition_fields(files, block_summary[tag]["groups"])

    return block_summary


def _build_group_summary(files: list[dict[str, Any]], group_by: str) -> dict[str, Any]:
    grouped_files: dict[str, list[dict[str, Any]]] = {}
    for file_record in files:
        value = file_record.get("capture_metadata", {}).get(group_by)
        if value is None:
            value = "<missing>"
        grouped_files.setdefault(str(value), []).append(file_record)

    result: dict[str, Any] = {}
    for group_value, group_files in sorted(grouped_files.items(), key=lambda item: (item[0] == "<missing>", item[0])):
        result[group_value] = {
            "file_count": len(group_files),
            "paths": [file_record["path"] for file_record in group_files],
            "block_summary": _build_block_summary(group_files),
        }
    return result


def _build_group_overlap_summary(files: list[dict[str, Any]], group_by: str) -> dict[str, Any]:
    summary: dict[str, Any] = {}
    for tag in OLYMPUS_UNKNOWN_BLOCK_TAGS:
        grouped_payloads: dict[str, dict[str, Any]] = {}
        for file_record in files:
            block = file_record["blocks"][tag]
            sha256 = block["sha256"]
            group_value = file_record.get("capture_metadata", {}).get(group_by)
            group_key = "<missing>" if group_value is None else str(group_value)
            entry = grouped_payloads.setdefault(
                sha256,
                {
                    "sha256": sha256,
                    "size_bytes": block["size_bytes"],
                    "groups": {},
                },
            )
            entry["groups"].setdefault(group_key, []).append(file_record["path"])

        shared_entries: list[dict[str, Any]] = []
        for entry in grouped_payloads.values():
            if len(entry["groups"]) < 2:
                continue
            shared_entries.append(
                {
                    "sha256": entry["sha256"],
                    "size_bytes": entry["size_bytes"],
                    "groups": dict(sorted(entry["groups"].items())),
                }
            )

        summary[tag] = {
            "shared_payload_count": len(shared_entries),
            "shared_payloads": sorted(shared_entries, key=lambda item: (len(item["groups"]), item["sha256"]), reverse=True),
        }
    return summary


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
    stacked_image = exiftool_data.get("Olympus:StackedImage")
    stacked_image_label = _decoded_stacked_image_label(path)
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
        "stacked_image": stacked_image,
        "stacked_image_label": stacked_image_label,
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


def inspect_olympus_makernote_blocks(path: Path, output_dir: Path | None = None) -> dict[str, Any]:
    exiftool_data = _run_exiftool_json(path)
    decoded_exiftool_data = _run_exiftool_json(path, numeric=False)
    stacked_image = exiftool_data.get("Olympus:StackedImage")
    stacked_image_label = _decoded_stacked_image_label(path, decoded_exiftool_data)
    block_output_dir = None
    if output_dir is not None:
        block_output_dir = output_dir / path.stem
        block_output_dir.mkdir(parents=True, exist_ok=True)

    blocks: dict[str, Any] = {}
    for tag in OLYMPUS_UNKNOWN_BLOCK_TAGS:
        payload = _run_exiftool_binary(path, tag)
        sha256 = hashlib.sha256(payload).hexdigest()
        block_record: dict[str, Any] = {
            "size_bytes": len(payload),
            "sha256": sha256,
            "payload": payload,
        }
        if block_output_dir is not None:
            output_path = block_output_dir / f"{tag}.bin"
            output_path.write_bytes(payload)
            block_record["output_path"] = str(output_path)
        blocks[tag] = block_record

    return {
        "path": str(path),
        "stacked_image": stacked_image,
        "stacked_image_label": stacked_image_label,
        "capture_metadata": _extract_correlation_metadata(exiftool_data, decoded_exiftool_data),
        "blocks": blocks,
    }


def _sanitize_makernote_record(file_record: dict[str, Any]) -> dict[str, Any]:
    sanitized_blocks: dict[str, Any] = {}
    for tag, block in file_record["blocks"].items():
        sanitized_blocks[tag] = {key: value for key, value in block.items() if key != "payload"}
    sanitized_record = dict(file_record)
    sanitized_record["blocks"] = sanitized_blocks
    return sanitized_record


def diff_olympus_makernote_blocks(source: Path, destination: Path) -> dict[str, Any]:
    source_record = inspect_olympus_makernote_blocks(source)
    destination_record = inspect_olympus_makernote_blocks(destination)

    block_diffs: dict[str, Any] = {}
    for tag in OLYMPUS_UNKNOWN_BLOCK_TAGS:
        source_block = source_record["blocks"][tag]
        destination_block = destination_record["blocks"][tag]
        source_payload = source_block["payload"]
        destination_payload = destination_block["payload"]
        diff_summary = _payload_diff_summary([source_payload, destination_payload])
        block_diffs[tag] = {
            "source_sha256": source_block["sha256"],
            "destination_sha256": destination_block["sha256"],
            "identical": source_block["sha256"] == destination_block["sha256"],
            "diff_summary": diff_summary,
        }

    return {
        "source": _sanitize_makernote_record(source_record),
        "destination": _sanitize_makernote_record(destination_record),
        "block_diffs": block_diffs,
    }


def inspect_olympus_makernote_block_set(
    paths: list[Path],
    output_dir: Path | None = None,
    group_by: str | None = None,
) -> dict[str, Any]:
    files = [inspect_olympus_makernote_blocks(path, output_dir=output_dir) for path in paths]

    block_summary = _build_block_summary(files)

    sanitized_files: list[dict[str, Any]] = []
    for file_record in files:
        sanitized_files.append(_sanitize_makernote_record(file_record))

    return {
        "file_count": len(sanitized_files),
        "files": sanitized_files,
        "block_summary": block_summary,
        "group_by": group_by,
        "group_summary": None if group_by is None else _build_group_summary(files, group_by),
        "group_overlap_summary": None if group_by is None else _build_group_overlap_summary(files, group_by),
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