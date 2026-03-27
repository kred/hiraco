from __future__ import annotations

import argparse
import json
from pathlib import Path

from hiraco import __version__
from hiraco.config import AppPaths, CompressionMode
from hiraco.metadata_copy import copy_metadata
from hiraco.metadata import compact_source_summary, extract_om3_stack_guidance
from hiraco.native_build import build_native_helper
from hiraco.native_contract import ConversionRequest
from hiraco.native_helper import NativeHelperMissingError, run_conversion

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="hiraco", description="Convert Olympus high-res ORI/ORF files to DNG.")
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    
    subparsers = parser.add_subparsers(dest="command", required=True)

    convert_parser = subparsers.add_parser("convert", help="Convert ORF/ORI to DNG")
    convert_parser.add_argument("source", type=Path, help="Input ORF/ORI file")
    convert_parser.add_argument("output", type=Path, help="Output DNG file")
    convert_parser.add_argument(
        "--compression",
        choices=[mode.value for mode in CompressionMode],
        default=CompressionMode.UNCOMPRESSED.value,
        help="Compression level"
    )
    convert_parser.add_argument(
        "--debug",
        action="store_true",
        help="Print verbose JSON diagnostic output"
    )
    convert_parser.set_defaults(handler=handle_convert)

    build_native_parser = subparsers.add_parser("build-native", help="Configure and build the native helper")
    build_native_parser.add_argument("--dng-sdk-root", type=Path, help="Override the Adobe DNG SDK root passed to CMake")
    build_native_parser.set_defaults(handler=handle_build_native)

    return parser


def _workspace_root() -> Path:
    return Path(__file__).resolve().parents[2]


def handle_convert(args: argparse.Namespace) -> int:
    if not args.source.exists():
        print(f"source file does not exist: {args.source}")
        return 2

    workspace_root = _workspace_root()
    paths = AppPaths.detect(workspace_root)
    try:
        source_info = compact_source_summary(args.source)
    except Exception as exc:
        print(f"preflight inspection failed: {exc}")
        return 2

    try:
        stack_guidance = extract_om3_stack_guidance(
            args.source,
            workspace_root / "tmp" / "stack_guidance",
        )
    except Exception as exc:
        print(f"stack guidance extraction failed: {exc}")
        return 2
    if stack_guidance:
        source_info.update(stack_guidance)

    request = ConversionRequest(
        source_path=args.source,
        output_path=args.output,
        compression=CompressionMode(args.compression),
        overwrite=True,
        source_info=source_info,
        predicted_detail_gain=1.8,
    )

    try:
        result = run_conversion(paths, request)
    except NativeHelperMissingError as exc:
        print(str(exc))
        return 2

    response = dict(result.__dict__)
    diagnostics = dict(response.get("diagnostics") or {})
    response["diagnostics"] = diagnostics

    if result.ok:
        metadata_result = copy_metadata(
            args.source,
            args.output,
            preserve_makernotes=False,
            dry_run=False,
        )
        diagnostics["metadata_copy"] = metadata_result
        if not metadata_result.get("ok"):
            response["ok"] = False
            response["message"] = "native DNG write succeeded but metadata copy failed"

    if args.debug:
        print(json.dumps(response, indent=2, sort_keys=True))
    else:
        if response.get("ok"):
            print(f"Success: {args.output}")
        else:
            print(f"Error: {response.get('message', 'Unknown error module')}")
            if "stderr" in diagnostics:
                print(f"Details: {diagnostics['stderr']}")

    return 0 if response.get("ok") else 1


def handle_build_native(args: argparse.Namespace) -> int:
    return build_native_helper(
        _workspace_root(),
        dng_sdk_root=args.dng_sdk_root,
    )


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.handler(args)
