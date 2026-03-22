from __future__ import annotations

from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from hiraco.config import CompressionMode


@dataclass(frozen=True)
class ConversionRequest:
    source_path: Path
    output_path: Path
    compression: CompressionMode
    overwrite: bool = False
    preserve_all_metadata: bool = True
    validate_output: bool = True
    source_info: dict[str, Any] | None = None

    def to_payload(self) -> dict[str, Any]:
        payload = asdict(self)
        payload["source_path"] = str(self.source_path)
        payload["output_path"] = str(self.output_path)
        payload["compression"] = self.compression.value
        return payload


@dataclass(frozen=True)
class NativeHelperResult:
    ok: bool
    message: str
    output_path: str | None = None
    diagnostics: dict[str, Any] | None = None
