from __future__ import annotations

import platform
import shutil
import subprocess
from pathlib import Path


def build_native_helper(
    workspace_root: Path,
    *,
    dng_sdk_root: Path | None = None,
) -> int:
    cmake = shutil.which("cmake")
    if cmake is None:
        print("cmake not found in PATH")
        return 2

    native_root = workspace_root / "native"
    build_root = native_root / "build"
    build_root.mkdir(parents=True, exist_ok=True)
    effective_dng_sdk_root = dng_sdk_root or (workspace_root / "dng_sdk_1_7_1")

    configure_command = [cmake, "-S", str(native_root), "-B", str(build_root)]
    configure_command.append(f"-DHIRACO_DNG_SDK_ROOT={effective_dng_sdk_root}")
    if platform.system() == "Darwin":
        configure_command.append(f"-DCMAKE_OSX_ARCHITECTURES={platform.machine()}")

    configure = subprocess.run(
        configure_command,
        check=False,
    )
    if configure.returncode != 0:
        return configure.returncode

    build = subprocess.run(
        [cmake, "--build", str(build_root)],
        check=False,
    )
    return build.returncode