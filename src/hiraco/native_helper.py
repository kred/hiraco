from __future__ import annotations

import json
import subprocess
from pathlib import Path

from hiraco.config import AppPaths
from hiraco.native_contract import ConversionRequest, NativeHelperResult


class NativeHelperMissingError(RuntimeError):
    pass


def run_conversion(paths: AppPaths, request: ConversionRequest) -> NativeHelperResult:
    helper = paths.native_helper_path
    if not helper.exists():
        raise NativeHelperMissingError(
            f"Native helper not found at {helper}. Build step is not implemented yet."
        )

    completed = subprocess.run(
        [str(helper), "convert", "--request-json", json.dumps(request.to_payload())],
        capture_output=True,
        text=True,
        check=False,
    )

    parsed_stdout = None
    if completed.stdout.strip():
        try:
            parsed_stdout = json.loads(completed.stdout)
        except json.JSONDecodeError:
            parsed_stdout = None

    if completed.returncode != 0 and parsed_stdout is None:
        return NativeHelperResult(
            ok=False,
            message="Native helper failed",
            diagnostics={
                "returncode": completed.returncode,
                "stdout": completed.stdout,
                "stderr": completed.stderr,
            },
        )

    data = parsed_stdout or {}
    return NativeHelperResult(
        ok=bool(data.get("ok")),
        message=str(data.get("message", "")),
        output_path=data.get("output_path"),
        diagnostics={
            "returncode": completed.returncode,
            "stderr": completed.stderr,
            "native": data.get("diagnostics"),
        },
    )
