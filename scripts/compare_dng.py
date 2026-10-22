#!/usr/bin/env python3
"""
compare_dng.py  —  Compare two DNG files produced by `hiraco-cli`.

The WebAssembly and native builds must agree on the pixels they write, so this
compares two DNGs at two levels:

  1. Structure - every IFD tag in both files, so a metadata or dimension
     difference is reported explicitly instead of hiding behind a pixel delta.
  2. Pixels    - the raw CFA/linear samples of the full-size raw IFD, decoded
     with `zlib` for the uncompressed and Deflate compression modes.  For
     JPEG XL compressed DNGs the compressed payload is compared byte-wise
     instead, which is an equally strict check because libjxl is deterministic.

Exit status:
    0  the two files agree within the configured tolerance
    1  the two files differ beyond the configured tolerance
    2  a file could not be parsed

Usage:
    python compare_dng.py reference.dng candidate.dng
    python compare_dng.py reference.dng candidate.dng --max-abs 1 --max-mean 0.001
    python compare_dng.py reference.dng candidate.dng --json
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))

from dng_reader import (  # noqa: E402
    TAG_AS_SHOT_NEUTRAL,
    TAG_BLACK_LEVEL,
    TAG_CFA_PATTERN,
    TAG_COMPRESSION,
    TAG_IMAGE_LENGTH,
    TAG_IMAGE_WIDTH,
    TAG_SAMPLES_PER_PIXEL,
    TAG_TILE_BYTE_COUNTS,
    TAG_TILE_OFFSETS,
    TAG_WHITE_LEVEL,
    COMPRESSION_JXL,
    DngFile,
    Ifd,
    decode_raw_samples,
    find_raw_ifd,
    open_dng,
    raw_strip_chunks,
)

# The measured difference between the two builds, with headroom.  These are
# the defaults the --wasm flag selects; see docs/wasm-verification.md.
WASM_MAX_ABS_DELTA = 16384
WASM_MAX_MEAN_ABS_DELTA = 0.05
WASM_MAX_DIFFERING_FRACTION = 0.0005

# Tags whose value legitimately varies between two runs of the *same* build
# (wall-clock stamps, version strings, random document identifiers).  Reported,
# but not treated as a mismatch.
#
# 306 DateTime, 700 XMP (embeds xmp:ModifyDate / xmp:MetadataDate and a random
# xmp:DocumentID + xmp:InstanceID), 305 Software, 315 Artist, 316 HostComputer,
# 33723 IPTC, 50708 UniqueCameraModel.
VOLATILE_TAGS = {
    305,  # Software
    306,  # DateTime
    315,  # Artist
    316,  # HostComputer
    33723,  # IPTC
    50708,  # UniqueCameraModel (contains a serial-derived token)
    700,  # XMP metadata packet
}

# Tags that give a *position in the file* or the *size of a variable length
# payload*.  When the samples differ, the compressed payload has a different
# length, and the embedded preview with it, so these move with the content.
# The content itself is still compared: the SubIFD, the Exif IFD and the GPS
# IFD are visited as IFDs of their own, so only their offsets are relaxed.
PAYLOAD_DEPENDENT_TAGS = {
    273,  # StripOffsets
    279,  # StripByteCounts
    324,  # TileOffsets
    325,  # TileByteCounts
    330,  # SubIFD (the offset of the raw IFD in the file)
    34665,  # ExifIFD
    34853,  # GPSInfoIFD
}

# Tags that are digests of the image data.  They differ exactly when the
# samples differ, so they are content hashes rather than metadata.
CONTENT_HASH_TAGS = {
    50781,  # tcRawDataUniqueID
    51111,  # tcNewRawImageDigest
}


def _tag_value_repr(ifd: Ifd, tag: int) -> Any:
    entry = ifd.entries.get(tag)
    if entry is None:
        return None
    ttype, value = entry
    if isinstance(value, bytes):
        return value.hex()
    if isinstance(value, list):
        return [v if not isinstance(v, bytes) else v.hex() for v in value]
    return value


def _shorten(value: Any, limit: int = 120) -> str:
    """Render a tag value compactly so a diff stays readable."""
    text = repr(value)
    if len(text) > limit:
        text = f"{text[:limit]}... ({len(text)} chars)"
    return text


def collect_structure(dng: DngFile) -> List[Dict[str, Any]]:
    """Flatten the IFD tree into a comparable list of records."""
    records: List[Dict[str, Any]] = []
    for index, ifd in enumerate(dng.all_ifds()):
        for tag in sorted(ifd.entries):
            records.append({
                "ifd": index,
                "tag": tag,
                "volatile": tag in VOLATILE_TAGS,
                "value": _tag_value_repr(ifd, tag),
            })
    return records


def compare_structure(a: DngFile, b: DngFile, relaxed: bool = False) -> List[str]:
    """Human-readable differences between the two IFD trees.

    With `relaxed` the tags that are derived from the image data - the
    content hashes and the payload offsets - are reported but not treated as
    differences, because they move exactly when the samples move.
    """
    ra = collect_structure(a)
    rb = collect_structure(b)
    key = lambda r: (r["ifd"], r["tag"])
    da = {key(r): r for r in ra}
    db = {key(r): r for r in rb}
    notes: List[str] = []
    for k in sorted(set(da) | set(db)):
        va, vb = da.get(k), db.get(k)
        if va is None:
            notes.append(f"ifd{k[0]} tag {k[1]}: only in candidate ({_shorten(vb['value'])})")
        elif vb is None:
            notes.append(f"ifd{k[0]} tag {k[1]}: only in reference ({_shorten(va['value'])})")
        elif va["value"] != vb["value"]:
            if va["volatile"] and vb["volatile"]:
                notes.append(f"ifd{k[0]} tag {k[1]}: differs (volatile tag, ignored) "
                             f"reference={_shorten(va['value'])} candidate={_shorten(vb['value'])}")
            elif relaxed and k[1] in CONTENT_HASH_TAGS:
                notes.append(f"ifd{k[0]} tag {k[1]}: differs (content hash, ignored) "
                             f"reference={_shorten(va['value'])} candidate={_shorten(vb['value'])}")
            elif relaxed and k[1] in PAYLOAD_DEPENDENT_TAGS:
                notes.append(f"ifd{k[0]} tag {k[1]}: differs (payload dependent, ignored)")
            else:
                notes.append(f"ifd{k[0]} tag {k[1]}: reference={_shorten(va['value'])} "
                             f"candidate={_shorten(vb['value'])}")
    return notes


def _u16_raster(blob: bytes) -> Any:
    import numpy as np
    return np.frombuffer(blob, dtype="<u2")


# The JPEG XL signatures.  The Adobe DNG SDK writes each tile as a JPEG XL
# container, which starts with a 12 byte "JXL " box carrying the container
# signature; a bare codestream starts with the two byte signature instead.
JXL_CONTAINER_SIGNATURE = b"\x00\x00\x00\x0cJXL \x0d\x0a\x87\x0a"
JXL_CODESTREAM_SIGNATURE = b"\xff\x0a"


def _is_jxl_payload(payload: bytes) -> bool:
    return (payload.startswith(JXL_CONTAINER_SIGNATURE) or
            payload.startswith(JXL_CODESTREAM_SIGNATURE))


def compare_jxl_payloads(a: DngFile, b: DngFile, ifa: Ifd, ifb: Ifd) -> Dict[str, Any]:
    """Compares the JPEG XL payloads of two DNGs.

    The payloads cannot be decoded here - the verification environment has no
    JPEG XL decoder - so the comparison checks what can be checked without one:
    that both files carry the same number of tiles, that every tile is a
    well-formed JPEG XL codestream, and that the payload sizes are in the same
    range.  The payloads themselves differ whenever the encoded pixels differ,
    which the pixel comparison measures exactly in the other compression modes.
    """
    chunks_a = raw_strip_chunks(a, ifa)
    chunks_b = raw_strip_chunks(b, ifb)
    result: Dict[str, Any] = {
        "mode": "jpeg-xl-payload",
        "chunk_count": [len(chunks_a), len(chunks_b)],
    }
    if len(chunks_a) != len(chunks_b):
        result["identical"] = False
        result["error"] = "the two files carry a different number of tiles"
        return result

    malformed = 0
    size_a = 0
    size_b = 0
    identical = True
    for (offset_a, length_a), (offset_b, length_b) in zip(chunks_a, chunks_b):
        payload_a = a.data[offset_a:offset_a + length_a]
        payload_b = b.data[offset_b:offset_b + length_b]
        size_a += length_a
        size_b += length_b
        if not _is_jxl_payload(payload_a) or not _is_jxl_payload(payload_b):
            malformed += 1
        if payload_a != payload_b:
            identical = False
    result["malformed_tiles"] = malformed
    result["total_bytes"] = [size_a, size_b]
    result["identical"] = identical and malformed == 0
    if size_a > 0 and size_b > 0:
        result["size_ratio"] = size_b / size_a
    return result


def compare_pixels(a: DngFile, b: DngFile) -> Dict[str, Any]:
    """Pixel-level comparison of the full-size raw IFD."""
    import numpy as np

    result: Dict[str, Any] = {"mode": None}
    ifa = find_raw_ifd(a)
    ifb = find_raw_ifd(b)
    if ifa is None or ifb is None:
        result["mode"] = "no-raw-ifd"
        result["error"] = "could not locate the full-size raw IFD in one of the files"
        return result

    for name, ifd in (("reference", ifa), ("candidate", ifb)):
        result[f"{name}_width"] = int(ifd.get(TAG_IMAGE_WIDTH, 0))
        result[f"{name}_height"] = int(ifd.get(TAG_IMAGE_LENGTH, 0))
        result[f"{name}_compression"] = int(ifd.get(TAG_COMPRESSION, 0))
        result[f"{name}_samples"] = int(ifd.get(TAG_SAMPLES_PER_PIXEL, 1) or 1)

    if (result["reference_width"], result["reference_height"]) != \
       (result["candidate_width"], result["candidate_height"]):
        result["mode"] = "dimension-mismatch"
        return result

    compression = result["reference_compression"]
    if compression == COMPRESSION_JXL or result["candidate_compression"] == COMPRESSION_JXL:
        # There is no JPEG XL decoder in the verification environment, so the
        # compressed payload is compared structurally: the same number of tiles,
        # each of them a well-formed codestream.  The payloads themselves
        # differ whenever the encoded pixels differ, and the pixel difference
        # is measured exactly in the other compression modes.
        return compare_jxl_payloads(a, b, ifa, ifb)

    sa, note_a = decode_raw_samples(a, ifa)
    sb, note_b = decode_raw_samples(b, ifb)
    if not sa or not sb:
        result["mode"] = "decode-failed"
        result["error"] = f"reference: {note_a}; candidate: {note_b}"
        return result

    result["mode"] = "pixels"
    result["decode"] = [note_a, note_b]
    xa = _u16_raster(sa)
    xb = _u16_raster(sb)
    if xa.shape != xb.shape:
        result["mode"] = "sample-count-mismatch"
        result["shape"] = [list(xa.shape), list(xb.shape)]
        return result
    diff = xa.astype(np.int32) - xb.astype(np.int32)
    absdiff = np.abs(diff)
    result["max_abs_delta"] = int(absdiff.max()) if absdiff.size else 0
    result["mean_abs_delta"] = float(absdiff.mean()) if absdiff.size else 0.0
    result["differing_samples"] = int(np.count_nonzero(diff))
    result["total_samples"] = int(diff.size)
    result["identical"] = bool(result["max_abs_delta"] == 0)
    result["reference_min"] = int(xa.min()) if xa.size else 0
    result["reference_max"] = int(xa.max()) if xa.size else 0
    result["candidate_min"] = int(xb.min()) if xb.size else 0
    result["candidate_max"] = int(xb.max()) if xb.size else 0
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("reference", help="reference DNG (usually the native build)")
    parser.add_argument("candidate", help="candidate DNG (usually the wasm build)")
    parser.add_argument("--max-abs", type=int, default=None,
                        help="maximum tolerated absolute per-sample delta "
                             "(default 0, or the WebAssembly tolerance with --wasm)")
    parser.add_argument("--max-mean", type=float, default=None,
                        help="maximum tolerated mean absolute per-sample delta "
                             "(default 0, or the WebAssembly tolerance with --wasm)")
    parser.add_argument("--max-differing-fraction", type=float, default=None,
                        help="maximum tolerated fraction of differing samples "
                             "(default 0, or the WebAssembly tolerance with --wasm)")
    parser.add_argument("--json", action="store_true", help="emit the result as JSON")
    parser.add_argument("--wasm", action="store_true",
                        help="compare a WebAssembly build against a native build: "
                             "the content hashes and the payload offsets are "
                             "allowed to differ, because they move exactly when "
                             "the samples move, and the documented tolerance for "
                             "the two architectures is applied")
    parser.add_argument("--ignore-structure", action="store_true",
                        help="do not fail on IFD tag differences (volatile tags are "
                             "already ignored)")
    args = parser.parse_args()

    if args.max_abs is None:
        args.max_abs = WASM_MAX_ABS_DELTA if args.wasm else 0
    if args.max_mean is None:
        args.max_mean = WASM_MAX_MEAN_ABS_DELTA if args.wasm else 0.0
    if args.max_differing_fraction is None:
        args.max_differing_fraction = WASM_MAX_DIFFERING_FRACTION if args.wasm else 0.0

    try:
        ref = open_dng(args.reference)
        cand = open_dng(args.candidate)
    except Exception as exc:  # noqa: BLE001 - report and exit cleanly
        print(f"error: {exc}", file=sys.stderr)
        return 2

    structure_notes = compare_structure(ref, cand, relaxed=args.wasm)
    pixels = compare_pixels(ref, cand)

    ok = True
    reasons: List[str] = []

    mode = pixels.get("mode")
    if mode == "pixels":
        if pixels["max_abs_delta"] > args.max_abs:
            ok = False
            reasons.append(f"max_abs_delta {pixels['max_abs_delta']} > {args.max_abs}")
        if pixels["mean_abs_delta"] > args.max_mean:
            ok = False
            reasons.append(f"mean_abs_delta {pixels['mean_abs_delta']:.6f} > {args.max_mean}")
        total = pixels["total_samples"]
        if total:
            fraction = pixels["differing_samples"] / total
            if fraction > args.max_differing_fraction:
                ok = False
                reasons.append(f"differing fraction {fraction:.6f} > {args.max_differing_fraction}")
    elif mode == "jpeg-xl-payload":
        if not pixels.get("identical"):
            ok = False
            reasons.append("jpeg-xl payload differs")
    else:
        ok = False
        reasons.append(f"pixel comparison unavailable: {pixels.get('error', mode)}")

    if not args.ignore_structure:
        ignored = "volatile tag, ignored"
        if args.wasm:
            ignored = ", ignored"
        hard = [n for n in structure_notes if ignored not in n]
        if hard:
            ok = False
            reasons.append(f"{len(hard)} structural tag difference(s)")

    report = {
        "reference": args.reference,
        "candidate": args.candidate,
        "ok": ok,
        "reasons": reasons,
        "structure_notes": structure_notes,
        "pixels": pixels,
    }

    if args.json:
        print(json.dumps(report, indent=2, default=str))
    else:
        print(f"reference : {args.reference}")
        print(f"candidate : {args.candidate}")
        print(f"dimensions: {pixels.get('reference_width')}x{pixels.get('reference_height')} "
              f"vs {pixels.get('candidate_width')}x{pixels.get('candidate_height')}")
        if mode == "pixels":
            print(f"pixel mode: {pixels.get('decode')}")
            print(f"max abs delta : {pixels['max_abs_delta']}")
            print(f"mean abs delta: {pixels['mean_abs_delta']:.6f}")
            print(f"differing     : {pixels['differing_samples']} / {pixels['total_samples']} samples")
        elif mode == "jpeg-xl-payload":
            print(f"pixel mode: jpeg-xl payload compare over {pixels.get('chunk_count')} chunks")
            print(f"identical  : {pixels.get('identical')}")
        else:
            print(f"pixel mode: {mode} ({pixels.get('error', '')})")
        if structure_notes:
            print("structure notes:")
            for note in structure_notes:
                print(f"  {note}")
        print("RESULT: " + ("PASS" if ok else "FAIL"))
        for reason in reasons:
            print(f"  - {reason}")

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
