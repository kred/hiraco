from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Any

from hiraco.config import AppPaths
from hiraco.native_contract import ConversionRequest, NativeHelperResult


class NativeHelperMissingError(RuntimeError):
    pass


def _run_helper_json(paths: AppPaths, command: str, payload: dict[str, Any]) -> tuple[int, str, str, dict[str, Any] | None]:
    helper = paths.native_helper_path
    if not helper.exists():
        raise NativeHelperMissingError(
            f"Native helper not found at {helper}. Build step is not implemented yet."
        )

    completed = subprocess.run(
        [str(helper), command, "--request-json", json.dumps(payload)],
        capture_output=True,
        text=True,
        check=False,
    )

    parsed_stdout: dict[str, Any] | None = None
    if completed.stdout.strip():
        try:
            parsed_stdout = json.loads(completed.stdout)
        except json.JSONDecodeError:
            parsed_stdout = None

    return completed.returncode, completed.stdout, completed.stderr, parsed_stdout


def run_conversion(paths: AppPaths, request: ConversionRequest) -> NativeHelperResult:
    completed_returncode, completed_stdout, completed_stderr, parsed_stdout = _run_helper_json(
        paths,
        "convert",
        request.to_payload(),
    )

    if completed_returncode != 0 and parsed_stdout is None:
        return NativeHelperResult(
            ok=False,
            message="Native helper failed",
            diagnostics={
                "returncode": completed_returncode,
                "stdout": completed_stdout,
                "stderr": completed_stderr,
            },
        )

    data = parsed_stdout or {}
    return NativeHelperResult(
        ok=bool(data.get("ok")),
        message=str(data.get("message", "")),
        output_path=data.get("output_path"),
        diagnostics={
            "returncode": completed_returncode,
            "stderr": completed_stderr,
            "native": data.get("diagnostics"),
        },
    )


