from __future__ import annotations

import argparse
import json
from pathlib import Path

from hiraco import __version__
from hiraco.config import AppPaths, CompressionMode
from hiraco.metadata_copy import copy_metadata
from hiraco.freshness import get_local_status, safe_get_upstream_status
from hiraco.metadata import MetadataToolError, compact_source_summary, inspect_file, metadata_diff
from hiraco.native_build import build_native_helper
from hiraco.native_contract import ConversionRequest
from hiraco.native_helper import NativeHelperMissingError, run_conversion
from hiraco.native_probe import run_native_probe


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="hiraco")
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")

    subparsers = parser.add_subparsers(dest="command", required=True)

    doctor_parser = subparsers.add_parser("doctor", help="Inspect local toolchain and optional upstream releases")
    doctor_parser.add_argument(
        "--check-upstream",
        action="store_true",
        help="Query upstream release sources for rawpy, ExifTool, LibRaw, and Adobe DNG SDK",
    )
    doctor_parser.set_defaults(handler=handle_doctor)

    convert_parser = subparsers.add_parser("convert", help="Convert ORF to DNG through the native helper")
    convert_parser.add_argument("source", type=Path)
    convert_parser.add_argument("output", type=Path)
    convert_parser.add_argument(
        "--compression",
        choices=[mode.value for mode in CompressionMode],
        default=CompressionMode.UNCOMPRESSED.value,
    )
    convert_parser.add_argument("--overwrite", action="store_true")
    convert_parser.add_argument("--preflight-only", action="store_true", help="Validate inputs and inspect source without invoking the native helper")
    convert_parser.set_defaults(handler=handle_convert)

    inspect_parser = subparsers.add_parser("inspect", help="Inspect a RAW file with rawpy and ExifTool")
    inspect_parser.add_argument("source", type=Path)
    inspect_parser.add_argument("--json", action="store_true", help="Print full JSON inspection payload")
    inspect_parser.set_defaults(handler=handle_inspect)

    native_probe_parser = subparsers.add_parser("probe-native", help="Inspect native LibRaw support for a source file")
    native_probe_parser.add_argument("source", type=Path)
    native_probe_parser.set_defaults(handler=handle_probe_native)

    diff_parser = subparsers.add_parser("metadata-diff", help="Compare metadata between two files using ExifTool")
    diff_parser.add_argument("source", type=Path)
    diff_parser.add_argument("destination", type=Path)
    diff_parser.add_argument("--json", action="store_true", help="Print full JSON diff payload")
    diff_parser.add_argument("--limit", type=int, default=20, help="Maximum number of diff keys to print in text mode")
    diff_parser.set_defaults(handler=handle_metadata_diff)

    copy_parser = subparsers.add_parser("copy-metadata", help="Copy EXIF/IPTC/XMP and optional MakerNotes with ExifTool")
    copy_parser.add_argument("source", type=Path)
    copy_parser.add_argument("destination", type=Path)
    copy_parser.add_argument("--dry-run", action="store_true", help="Print the ExifTool command and skip the write")
    copy_parser.add_argument("--no-makernotes", action="store_true", help="Skip MakerNotes copy")
    copy_parser.set_defaults(handler=handle_copy_metadata)

    build_native_parser = subparsers.add_parser("build-native", help="Configure and build the native helper")
    build_native_parser.add_argument("--enable-dng-sdk", action="store_true", help="Enable Adobe DNG SDK header integration scaffold")
    build_native_parser.add_argument("--dng-sdk-root", type=Path, help="Override the Adobe DNG SDK root passed to CMake")
    build_native_parser.set_defaults(handler=handle_build_native)

    contract_parser = subparsers.add_parser("print-contract", help="Print the native helper request schema example")
    contract_parser.set_defaults(handler=handle_print_contract)

    return parser


def _workspace_root() -> Path:
    return Path(__file__).resolve().parents[2]


def handle_doctor(args: argparse.Namespace) -> int:
    workspace_root = _workspace_root()
    local_status = get_local_status(workspace_root)

    print("Local status:")
    for status in local_status:
        print(f"- {status.name}: {status.local or 'not found'} ({status.details})")

    paths = AppPaths.detect(workspace_root)
    print("- Native helper path:", paths.native_helper_path)
    print("- Adobe DNG SDK folder:", paths.dng_sdk_root)

    if args.check_upstream:
        upstream_status, errors = safe_get_upstream_status()
        if upstream_status:
            print("Upstream status:")
            for status in upstream_status:
                print(f"- {status.name}: {status.upstream or 'unknown'} ({status.details})")
        for error in errors:
            print(error)

    return 0


def handle_convert(args: argparse.Namespace) -> int:
    if not args.source.exists():
        print(f"source file does not exist: {args.source}")
        return 2
    if args.output.exists() and not args.overwrite:
        print(f"output file already exists: {args.output}")
        return 2

    workspace_root = _workspace_root()
    paths = AppPaths.detect(workspace_root)
    try:
        source_info = compact_source_summary(args.source)
    except Exception as exc:
        print(f"preflight inspection failed: {exc}")
        return 2

    request = ConversionRequest(
        source_path=args.source,
        output_path=args.output,
        compression=CompressionMode(args.compression),
        overwrite=args.overwrite,
        source_info=source_info,
    )

    if args.preflight_only:
        print(json.dumps(request.to_payload(), indent=2, sort_keys=True))
        return 0

    try:
        result = run_conversion(paths, request)
    except NativeHelperMissingError as exc:
        print(str(exc))
        return 2

    response = dict(result.__dict__)

    if result.ok:
        metadata_result = copy_metadata(
            args.source,
            args.output,
            preserve_makernotes=False,
            dry_run=False,
        )
        diagnostics = dict(response.get("diagnostics") or {})
        diagnostics["metadata_copy"] = metadata_result
        response["diagnostics"] = diagnostics
        if not metadata_result.get("ok"):
            response["ok"] = False
            response["message"] = "native DNG write succeeded but metadata copy failed"

    print(json.dumps(response, indent=2, sort_keys=True))
    return 0 if response.get("ok") else 1


def handle_build_native(args: argparse.Namespace) -> int:
    return build_native_helper(
        _workspace_root(),
        enable_dng_sdk=args.enable_dng_sdk,
        dng_sdk_root=args.dng_sdk_root,
    )


def handle_inspect(args: argparse.Namespace) -> int:
    if not args.source.exists():
        print(f"source file does not exist: {args.source}")
        return 2
    try:
        payload = inspect_file(args.source)
    except MetadataToolError as exc:
        print(str(exc))
        return 2

    if args.json:
        print(json.dumps(payload, indent=2, sort_keys=True))
        return 0

    summary = compact_source_summary(args.source)
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


def handle_probe_native(args: argparse.Namespace) -> int:
    workspace_root = _workspace_root()
    paths = AppPaths.detect(workspace_root)
    payload = run_native_probe(paths, args.source)
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0 if payload.get("ok") else 1


def handle_metadata_diff(args: argparse.Namespace) -> int:
    if not args.source.exists():
        print(f"source file does not exist: {args.source}")
        return 2
    if not args.destination.exists():
        print(f"destination file does not exist: {args.destination}")
        return 2

    try:
        payload = metadata_diff(args.source, args.destination)
    except MetadataToolError as exc:
        print(str(exc))
        return 2

    if args.json:
        print(json.dumps(payload, indent=2, sort_keys=True))
        return 0

    print(f"source tags: {payload['source_tag_count']}")
    print(f"destination tags: {payload['destination_tag_count']}")
    for label in ("missing_in_destination", "added_in_destination", "changed"):
        keys = list(payload[label].keys())
        print(f"{label}: {len(keys)}")
        for key in keys[: args.limit]:
            print(f"- {key}")
    return 0


def handle_copy_metadata(args: argparse.Namespace) -> int:
    if not args.source.exists():
        print(f"source file does not exist: {args.source}")
        return 2
    if not args.destination.exists() and not args.dry_run:
        print(f"destination file does not exist: {args.destination}")
        return 2

    payload = copy_metadata(
        args.source,
        args.destination,
        preserve_makernotes=not args.no_makernotes,
        dry_run=args.dry_run,
    )
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0 if payload.get("ok") else 1


def handle_print_contract(_: argparse.Namespace) -> int:
    example = ConversionRequest(
        source_path=Path("input.orf"),
        output_path=Path("output.dng"),
        compression=CompressionMode.DEFLATE,
    )
    print(json.dumps(example.to_payload(), indent=2, sort_keys=True))
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.handler(args)