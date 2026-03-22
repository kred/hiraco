from __future__ import annotations

import json
import shlex
import subprocess
from pathlib import Path


def build_copy_command(source: Path, destination: Path, preserve_makernotes: bool = True) -> list[str]:
    command = [
        "exiftool",
        "-overwrite_original",
        "-P",
        "-TagsFromFile",
        str(source),
        "-EXIF:all",
        "-IPTC:all",
        "-XMP:all",
        "--EXIF:Orientation",
        "--XMP-tiff:Orientation",
    ]
    if preserve_makernotes:
        command.append("-MakerNotes:all")
    else:
        command.extend([
            "--MakerNotes:all",
            "--Olympus:all",
        ])
    command.append(str(destination))
    return command


def format_copy_command(command: list[str]) -> str:
    return " ".join(shlex.quote(part) for part in command)


def build_cleanup_command(destination: Path) -> list[str]:
    return [
        "exiftool",
        "-overwrite_original",
        "-P",
        "-MakerNotes:all=",
        "-Olympus:all=",
        str(destination),
    ]


def _reference_dng_for_source(source: Path) -> Path | None:
    candidate = source.with_name(f"dxo_{source.stem.lstrip('_')}.dng")
    if candidate.exists():
        return candidate
    return None


def _build_reference_override_command(reference: Path, destination: Path) -> list[str]:
    return [
        "exiftool",
        "-overwrite_original",
        "-P",
        "-TagsFromFile",
        str(reference),
        "-XMP:all",
        "-IFD0:Orientation",
        "-IFD0:AsShotNeutral",
        "-IFD0:CalibrationIlluminant1",
        "-IFD0:CalibrationIlluminant2",
        "-IFD0:ColorMatrix1",
        "-IFD0:ColorMatrix2",
        "-IFD0:ForwardMatrix1",
        "-IFD0:ForwardMatrix2",
        "-IFD0:CameraCalibration1",
        "-IFD0:CameraCalibration2",
        "-IFD0:BaselineExposure",
        "-IFD0:BaselineNoise",
        "-IFD0:BaselineSharpness",
        "-IFD0:LinearResponseLimit",
        "-IFD0:Make",
        "-IFD0:Model",
        "-IFD0:UniqueCameraModel",
        "-IFD0:ProfileName",
        "-IFD0:ProfileCalibrationSig",
        "-IFD0:ProfileHueSatMapDims",
        "-IFD0:ProfileHueSatMapData1",
        "-IFD0:ProfileHueSatMapData2",
        "-IFD0:ProfileEmbedPolicy",
        "-IFD0:ProfileCopyright",
        "-IFD0:ProfileLookTableDims",
        "-IFD0:ProfileLookTableData",
        "-IFD0:NoiseProfile",
        "-SubIFD:DefaultCropOrigin",
        "-SubIFD:DefaultCropSize",
        "-SubIFD:OpcodeList3",
        "-SubIFD:NoiseProfile",
        "-SubIFD:WhiteLevel",
        "-SubIFD:BlackLevel=0 0 0",
        str(destination),
    ]


def _read_source_override_tags(source: Path) -> dict[str, object]:
    completed = subprocess.run(
        [
            "exiftool",
            "-j",
            "-n",
            "-Make",
            "-Model",
            "-BlackLevel2",
            "-ValidBits",
            "-CropLeft",
            "-CropTop",
            "-CropWidth",
            "-CropHeight",
            "-Orientation",
            str(source),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        return {}

    payload = json.loads(completed.stdout)
    if not payload:
        return {}
    return payload[0]


def _parse_space_separated_floats(value: object) -> list[float]:
    if isinstance(value, (int, float)):
        return [float(value)]
    if isinstance(value, (list, tuple)):
        return [float(item) for item in value]
    if isinstance(value, str):
        return [float(item) for item in value.split()]
    return []


def _parse_first_numeric(value: object, default: int = 0) -> int:
    values = _parse_space_separated_floats(value)
    if not values:
        return default
    return int(values[0])


def _build_dng_override_command(source: Path, destination: Path) -> list[str] | None:
    reference = _reference_dng_for_source(source)
    if reference is not None:
        return _build_reference_override_command(reference, destination)

    tags = _read_source_override_tags(source)
    command = [
        "exiftool",
        "-overwrite_original",
        "-P",
        "-n",
        "-ColorimetricReference=0",
    ]

    command.append("-Make=hiraco")
    command.append("-Model=linear-raw")
    command.append("-UniqueCameraModel=hiraco-linear-raw")

    black_levels = _parse_space_separated_floats(tags.get("BlackLevel2"))
    if len(black_levels) >= 3:
        scaled_black = [value * 4.0 for value in black_levels[:3]]
        command.append("-SubIFD:BlackLevel=" + " ".join(f"{value:.0f}" for value in scaled_black))

    valid_bits = _parse_first_numeric(tags.get("ValidBits"), 0)
    if valid_bits > 0:
        white_level = (1 << valid_bits) - 1
        command.append(f"-SubIFD:WhiteLevel={white_level} {white_level} {white_level}")

    if len(command) == 5:
        return None

    command.append(str(destination))
    return command


def copy_metadata(source: Path, destination: Path, preserve_makernotes: bool = True, dry_run: bool = False) -> dict[str, object]:
    command = build_copy_command(source, destination, preserve_makernotes=preserve_makernotes)
    cleanup_command = None if preserve_makernotes else build_cleanup_command(destination)
    override_command = None if preserve_makernotes else _build_dng_override_command(source, destination)
    if dry_run:
        payload = {
            "ok": True,
            "command": command,
            "command_text": format_copy_command(command),
            "dry_run": True,
        }
        if cleanup_command is not None:
            payload["cleanup_command"] = cleanup_command
            payload["cleanup_command_text"] = format_copy_command(cleanup_command)
        if override_command is not None:
            payload["override_command"] = override_command
            payload["override_command_text"] = format_copy_command(override_command)
        return payload

    completed = subprocess.run(
        command,
        capture_output=True,
        text=True,
        check=False,
    )
    payload = {
        "ok": completed.returncode == 0,
        "command": command,
        "command_text": format_copy_command(command),
        "dry_run": False,
        "returncode": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }
    if cleanup_command is None or completed.returncode != 0:
        return payload

    cleanup_completed = subprocess.run(
        cleanup_command,
        capture_output=True,
        text=True,
        check=False,
    )
    payload["cleanup_command"] = cleanup_command
    payload["cleanup_command_text"] = format_copy_command(cleanup_command)
    payload["cleanup_returncode"] = cleanup_completed.returncode
    payload["cleanup_stdout"] = cleanup_completed.stdout
    payload["cleanup_stderr"] = cleanup_completed.stderr
    payload["ok"] = payload["ok"] and cleanup_completed.returncode == 0

    if cleanup_completed.returncode != 0:
        return payload

    if override_command is None:
        return payload

    override_completed = subprocess.run(
        override_command,
        capture_output=True,
        text=True,
        check=False,
    )
    payload["override_command"] = override_command
    payload["override_command_text"] = format_copy_command(override_command)
    payload["override_returncode"] = override_completed.returncode
    payload["override_stdout"] = override_completed.stdout
    payload["override_stderr"] = override_completed.stderr
    payload["ok"] = payload["ok"] and override_completed.returncode == 0
    return payload