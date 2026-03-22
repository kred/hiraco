from __future__ import annotations

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


def copy_metadata(source: Path,
                  destination: Path,
                  preserve_makernotes: bool = True,
                  dry_run: bool = False) -> dict[str, object]:
    command = build_copy_command(source, destination, preserve_makernotes=preserve_makernotes)
    cleanup_command = None if preserve_makernotes else build_cleanup_command(destination)
    override_command = None
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