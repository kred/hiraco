from __future__ import annotations

import json
import subprocess
from pathlib import Path

from hiraco.config import AppPaths


def run_native_probe(paths: AppPaths, source: Path) -> dict[str, object]:
    helper = paths.native_helper_path
    if not helper.exists():
        return {
            "ok": False,
            "message": f"Native helper not found at {helper}",
            "diagnostics": None,
        }

    completed = subprocess.run(
        [str(helper), "probe", "--source", str(source)],
        capture_output=True,
        text=True,
        check=False,
    )

    payload: dict[str, object]
    try:
        payload = json.loads(completed.stdout or "{}")
    except json.JSONDecodeError:
        payload = {
            "ok": False,
            "message": "native probe returned invalid JSON",
            "diagnostics": {
                "returncode": completed.returncode,
                "stdout": completed.stdout,
                "stderr": completed.stderr,
            },
        }
        return payload

    diagnostics = payload.get("diagnostics")
    if isinstance(diagnostics, dict):
        diagnostics = {
            **diagnostics,
            "returncode": completed.returncode,
            "stderr": completed.stderr,
        }
    payload["diagnostics"] = diagnostics
    return payload