from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from pathlib import Path


class CompressionMode(str, Enum):
    UNCOMPRESSED = "uncompressed"
    DEFLATE = "deflate"
    JPEG_XL = "jpeg-xl"


@dataclass(frozen=True)
class AppPaths:
    workspace_root: Path
    dng_sdk_root: Path
    native_helper_path: Path

    @classmethod
    def detect(cls, workspace_root: Path) -> "AppPaths":
        dng_sdk_root = workspace_root / "dng_sdk_1_7_1"
        native_helper_path = workspace_root / "native" / "build" / "hiraco-native"
        return cls(
            workspace_root=workspace_root,
            dng_sdk_root=dng_sdk_root,
            native_helper_path=native_helper_path,
        )
