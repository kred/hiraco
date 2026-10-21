#!/usr/bin/env python3
"""
dng_reader.py  —  Dependency-free reader for classic-TIFF DNG files.

Extracts the IFD tag tree and, where the compression allows it, the raw pixel
samples.  It exists so the WebAssembly and native `hiraco-cli` builds can be
compared pixel-for-pixel without pulling in a RAW decoder: the Adobe DNG SDK
writes classic little-endian TIFF, and the two compression modes used by
`hiraco-cli` (uncompressed and Deflate) decode with nothing but `zlib`.

JPEG XL compressed DNGs (Compression = 52546) cannot be decoded here; for
those the reader reports the compressed strips so they can be compared
byte-for-byte instead.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple

# TIFF tag numbers the reader cares about.
TAG_NEW_SUBFILE_TYPE = 254
TAG_IMAGE_WIDTH = 256
TAG_IMAGE_LENGTH = 257
TAG_BITS_PER_SAMPLE = 258
TAG_COMPRESSION = 259
TAG_PHOTOMETRIC = 262
TAG_STRIP_OFFSETS = 273
TAG_SAMPLES_PER_PIXEL = 277
TAG_ROWS_PER_STRIP = 278
TAG_STRIP_BYTE_COUNTS = 279
TAG_PLANAR_CONFIG = 284
TAG_TILE_WIDTH = 322
TAG_TILE_LENGTH = 323
TAG_TILE_OFFSETS = 324
TAG_TILE_BYTE_COUNTS = 325
TAG_SUB_IFD = 330
TAG_CFA_REPEAT_PATTERN_DIM = 33421
TAG_CFA_PATTERN = 50710
TAG_LINEARIZATION_TABLE = 50712
TAG_BLACK_LEVEL = 50714
TAG_WHITE_LEVEL = 50717
TAG_ACTIVE_AREA = 50829
TAG_AS_SHOT_NEUTRAL = 50728
TAG_UNIQUE_CAMERA_MODEL = 50708

COMPRESSION_UNCOMPRESSED = 1
COMPRESSION_JPEG = 7
COMPRESSION_DEFLATE = 8
COMPRESSION_JXL = 52546

TYPE_SIZES = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4, 10: 8,
               11: 4, 12: 8, 13: 4, 16: 8, 17: 8, 18: 8}
TYPE_NAMES = {1: "BYTE", 2: "ASCII", 3: "SHORT", 4: "LONG", 5: "RATIONAL",
              6: "SBYTE", 7: "UNDEFINED", 8: "SSHORT", 9: "SLONG", 10: "SRATIONAL",
              11: "FLOAT", 12: "DOUBLE", 13: "IFD", 16: "LONG8", 17: "SLONG8",
              18: "IFD8"}

# Tags whose value legitimately changes between two runs of the same input.
# `DateTime` and friends are wall-clock stamps; the DNG SDK also writes a
# unique-ish software/version stamp.  These are excluded from the strict
# structural comparison but still reported.
VOLATILE_TAGS = {306, 305, 315, 316, 33723, 50708, 50711}


class DngParseError(RuntimeError):
    pass


@dataclass
class Ifd:
    """One TIFF image file directory."""

    offset: int
    entries: Dict[int, Tuple[int, Any]] = field(default_factory=dict)
    sub_ifds: List[int] = field(default_factory=list)
    next_ifd: int = 0

    def get(self, tag: int, default: Any = None) -> Any:
        return self.entries.get(tag, (0, default))[1]

    def has(self, tag: int) -> bool:
        return tag in self.entries

    def describe(self) -> str:
        bits = []
        for tag in sorted(self.entries):
            ttype, value = self.entries[tag]
            if isinstance(value, bytes):
                shown = f"<{len(value)} bytes>"
            elif isinstance(value, str):
                shown = repr(value)
            elif isinstance(value, (list, tuple)):
                shown = str(list(value)[:8]) + (" ..." if len(value) > 8 else "")
            else:
                shown = str(value)
            bits.append(f"    {tag:>6} ({TYPE_NAMES.get(ttype, ttype):<9}) = {shown}")
        return "\n".join(bits)


@dataclass
class DngFile:
    path: str
    data: bytes
    ifds: Dict[int, Ifd] = field(default_factory=dict)
    byte_order: str = "<"
    first_ifd: int = 0

    def ifd(self, offset: int) -> Ifd:
        if offset in self.ifds:
            return self.ifds[offset]
        parsed = self._parse_ifd(offset)
        self.ifds[offset] = parsed
        return parsed

    def _read_values(self, entry_offset: int, ttype: int, count: int) -> Any:
        size = TYPE_SIZES.get(ttype)
        if size is None:
            return None
        total = size * count
        if total <= 4:
            raw = self.data[entry_offset:entry_offset + total]
        else:
            (value_offset,) = struct.unpack_from(self.byte_order + "I", self.data, entry_offset)
            raw = self.data[value_offset:value_offset + total]
        return self._decode(ttype, count, raw)

    def _decode(self, ttype: int, count: int, raw: bytes) -> Any:
        if ttype == 2:  # ASCII
            return raw.split(b"\0")[0].decode("latin-1")
        if ttype in (1, 6, 7):
            return raw
        fmt = {1: "B", 3: "H", 4: "I", 8: "h", 9: "i", 11: "f", 12: "d",
               13: "I", 16: "Q", 17: "q"}.get(ttype)
        if fmt is None:
            return raw
        values = list(struct.unpack(self.byte_order + fmt * count, raw[:struct.calcsize(fmt) * count]))
        if ttype in (5, 10) and values:
            pairs = [values[i * 2:i * 2 + 2] for i in range(count)]
            return [p[0] / p[1] if p[1] else 0.0 for p in pairs]
        if count == 1:
            return values[0]
        return values

    def _parse_ifd(self, offset: int) -> Ifd:
        if offset + 2 > len(self.data):
            raise DngParseError(f"IFD offset {offset} out of range")
        (count,) = struct.unpack_from(self.byte_order + "H", self.data, offset)
        ifd = Ifd(offset=offset)
        for i in range(count):
            entry = offset + 2 + i * 12
            if entry + 12 > len(self.data):
                raise DngParseError("truncated IFD")
            tag, ttype, n = struct.unpack_from(self.byte_order + "HHI", self.data, entry)
            value = self._read_values(entry + 8, ttype, n)
            ifd.entries[tag] = (ttype, value)
        next_offset = offset + 2 + count * 12
        if next_offset + 4 <= len(self.data):
            (ifd.next_ifd,) = struct.unpack_from(self.byte_order + "I", self.data, next_offset)
        sub = ifd.get(TAG_SUB_IFD)
        if sub is None:
            ifd.sub_ifds = []
        elif isinstance(sub, int):
            ifd.sub_ifds = [sub]
        else:
            ifd.sub_ifds = [int(v) for v in sub]
        return ifd

    def all_ifds(self) -> List[Ifd]:
        """IFD0 plus every reachable SubIFD, in traversal order."""
        seen: List[Ifd] = []
        visited = set()

        def visit(offset: int) -> None:
            if offset in visited or offset == 0:
                return
            visited.add(offset)
            ifd = self.ifd(offset)
            seen.append(ifd)
            for sub in ifd.sub_ifds:
                visit(sub)

        visit(self.first_ifd)
        return seen


def open_dng(path: str) -> DngFile:
    with open(path, "rb") as handle:
        data = handle.read()
    if len(data) < 8:
        raise DngParseError(f"{path}: too small to be a TIFF")
    order = data[:2]
    if order == b"II":
        byte_order = "<"
    elif order == b"MM":
        byte_order = ">"
    else:
        raise DngParseError(f"{path}: not a classic TIFF (byte order {order!r})")
    (magic,) = struct.unpack_from(byte_order + "H", data, 2)
    if magic == 42:
        ifd0 = 0
    elif magic == 43:  # BigTIFF - the DNG SDK does not write this.
        raise DngParseError(f"{path}: BigTIFF is not supported by this reader")
    else:
        raise DngParseError(f"{path}: bad TIFF magic {magic}")
    (first_ifd,) = struct.unpack_from(byte_order + "I", data, 4)
    dng = DngFile(path=path, data=data, byte_order=byte_order, first_ifd=first_ifd)
    if first_ifd:
        dng.ifd(first_ifd)
    return dng


def find_raw_ifd(dng: DngFile) -> Optional[Ifd]:
    """The IFD that carries the CFA/raw samples.

    The DNG SDK writes the raw data in a SubIFD whose NewSubFileType has the
    "reduced resolution" bit set for previews and unset for the full-size raw
    IFD.  Pick the candidate with the largest area that is not a preview.
    """
    candidates = []
    for ifd in dng.all_ifds():
        if not ifd.has(TAG_STRIP_OFFSETS) and not ifd.has(TAG_TILE_OFFSETS):
            continue
        width = ifd.get(TAG_IMAGE_WIDTH)
        height = ifd.get(TAG_IMAGE_LENGTH)
        if not width or not height:
            continue
        sub_type = ifd.get(TAG_NEW_SUBFILE_TYPE, 0) or 0
        is_preview = bool(sub_type & 1)
        candidates.append((not is_preview, int(width) * int(height), ifd))
    if not candidates:
        return None
    candidates.sort(key=lambda item: (item[0], item[1]), reverse=True)
    return candidates[0][2]


def _strip_payload(dng: DngFile, ifd: Ifd) -> Tuple[bytes, List[Tuple[int, int]]]:
    """Return (raw strip bytes, list of (offset, byte_count))."""
    offsets = ifd.get(TAG_STRIP_OFFSETS)
    counts = ifd.get(TAG_STRIP_BYTE_COUNTS)
    if offsets is None or counts is None:
        return b"", []
    if isinstance(offsets, int):
        offsets = [offsets]
    if isinstance(counts, int):
        counts = [counts]
    chunks = []
    for offset, count in zip(offsets, counts):
        chunks.append((int(offset), int(count)))
    return b"".join(dng.data[o:o + c] for o, c in chunks), chunks


def _tile_layout(ifd: Ifd) -> Optional[Tuple[int, int, int, int]]:
    """(tile_width, tile_height, tiles_across, tiles_down) for a tiled IFD."""
    tile_w = ifd.get(TAG_TILE_WIDTH)
    tile_h = ifd.get(TAG_TILE_LENGTH)
    width = ifd.get(TAG_IMAGE_WIDTH)
    height = ifd.get(TAG_IMAGE_LENGTH)
    if not tile_w or not tile_h or not width or not height:
        return None
    tile_w, tile_h = int(tile_w), int(tile_h)
    across = (int(width) + tile_w - 1) // tile_w
    down = (int(height) + tile_h - 1) // tile_h
    return tile_w, tile_h, across, down


def _tile_chunks(dng: DngFile, ifd: Ifd) -> List[Tuple[int, int]]:
    offsets = ifd.get(TAG_TILE_OFFSETS)
    counts = ifd.get(TAG_TILE_BYTE_COUNTS)
    if offsets is None or counts is None:
        return []
    if isinstance(offsets, int):
        offsets = [offsets]
    if isinstance(counts, int):
        counts = [counts]
    return [(int(o), int(c)) for o, c in zip(offsets, counts)]


def decode_raw_samples(dng: DngFile, ifd: Ifd) -> Tuple[bytes, str]:
    """Decode the raw samples of `ifd` into a contiguous raster.

    Returns (raster_bytes, note).  The raster holds little-endian unsigned
    16-bit samples in row-major order when the compression is supported;
    otherwise it is empty and `note` explains why.
    """
    compression = ifd.get(TAG_COMPRESSION, 1)
    width = int(ifd.get(TAG_IMAGE_WIDTH, 0))
    height = int(ifd.get(TAG_IMAGE_LENGTH, 0))
    bits = ifd.get(TAG_BITS_PER_SAMPLE, 16)
    if isinstance(bits, list):
        bits = bits[0]
    bits = int(bits)
    if bits != 16:
        return b"", f"unsupported BitsPerSample {bits}"
    samples_per_pixel = int(ifd.get(TAG_SAMPLES_PER_PIXEL, 1) or 1)
    row_bytes = width * samples_per_pixel * 2

    layout = _tile_layout(ifd)
    if layout is not None:
        tile_w, tile_h, across, down = layout
        tile_row_bytes = tile_w * samples_per_pixel * 2
        chunks = _tile_chunks(dng, ifd)
        if len(chunks) != across * down:
            return b"", f"tile count {len(chunks)} != {across}x{down}"
        raster = bytearray(row_bytes * height)
        for index, (offset, count) in enumerate(chunks):
            blob = dng.data[offset:offset + count]
            if compression == COMPRESSION_DEFLATE:
                try:
                    blob = zlib.decompress(blob)
                except zlib.error as exc:
                    return b"", f"deflate decode failed on tile {index}: {exc}"
            elif compression != COMPRESSION_UNCOMPRESSED:
                return b"", f"unsupported compression {compression}"
            if len(blob) < tile_row_bytes * tile_h:
                return b"", f"tile {index} decoded to {len(blob)} bytes, expected {tile_row_bytes * tile_h}"
            tile_index_col = index % across
            tile_index_row = index // across
            for row in range(tile_h):
                target_y = tile_index_row * tile_h + row
                if target_y >= height:
                    break
                src_start = row * tile_row_bytes
                copy_width = min(tile_w, width - tile_index_col * tile_w)
                if copy_width <= 0:
                    break
                dst_start = target_y * row_bytes + tile_index_col * tile_w * samples_per_pixel * 2
                raster[dst_start:dst_start + copy_width * samples_per_pixel * 2] = \
                    blob[src_start:src_start + copy_width * samples_per_pixel * 2]
        return bytes(raster), "tiled"

    payload, _ = _strip_payload(dng, ifd)
    if compression == COMPRESSION_UNCOMPRESSED:
        return payload, "stripped"
    if compression == COMPRESSION_DEFLATE:
        try:
            return zlib.decompress(payload), "deflate"
        except zlib.error as exc:
            return b"", f"deflate decode failed: {exc}"
    if compression == COMPRESSION_JXL:
        return b"", "jpeg-xl (payload compared byte-wise instead)"
    return b"", f"unsupported compression {compression}"


def raw_strip_chunks(dng: DngFile, ifd: Ifd) -> List[Tuple[int, int]]:
    """Compressed payload chunks (strips or tiles) with their byte ranges."""
    layout = _tile_layout(ifd)
    if layout is not None:
        return _tile_chunks(dng, ifd)
    return _strip_payload(dng, ifd)[1]
