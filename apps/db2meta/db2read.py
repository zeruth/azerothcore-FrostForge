#!/usr/bin/env python3
#
# This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
#
"""Reads 3.4.3.54261 WDC4 client tables, for analysis scripts.

A port of src/common/DataStores/DB2FileLoader.cpp, described by the same metadata
(src/server/shared/DataStores/DB2Metadata.h). Values come back per record as a list of
columns, each a list of its array elements: ints (sign-extended as declared), floats, strings.

    python apps/db2meta/db2read.py Map [--dbc var/data/dbc] [--id 0 --id 571] [--limit 5]
"""

from __future__ import annotations

import argparse
import re
import struct
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
METADATA = ROOT / "src/server/shared/DataStores/DB2Metadata.h"
DBC_DIR = ROOT / "var/data/dbc"

INT32, INT64, STRING, FLOAT, INT8, INT16 = "Int32", "Int64", "String", "Float", "Int8", "Int16"
WIDTH = {INT8: 1, INT16: 2, INT64: 8}

# storage of a column in regular files
NONE, PACKED, COMMON, PALLET, PALLET_ARRAY, PACKED_SIGNED = range(6)


@dataclass
class Field:
    type: str
    array: int
    signed: bool
    localized: bool


@dataclass
class Meta:
    name: str
    file_data_id: int
    index_field: int
    parent_index_field: int
    field_count: int
    file_field_count: int
    table_hash: int
    layout_hash: int
    sparse: bool
    fields: list[Field]


_metas: dict[str, Meta] | None = None


def metas() -> dict[str, Meta]:
    global _metas
    if _metas is None:
        text = METADATA.read_text(encoding="utf-8")
        fields = {}
        for m in re.finditer(r"DB2MetaField (\w+)Fields\[\] =\s*\{(.*?)\};", text, re.S):
            fields[m.group(1)] = [Field(t, int(a), s == "true", l == "true") for t, a, s, l in re.findall(
                r"\{ DB2FieldType::(\w+), (\d+), (\w+), (\w+) \}", m.group(2))]
        _metas = {}
        for m in re.finditer(r'\{ "(\w+)", (\d+), (-?\d+), (-?\d+), (\d+), (\d+), (0x[0-9A-F]+), (0x[0-9A-F]+),\s*(\w+), (\w+)Fields \}', text):
            name = m.group(1)
            _metas[name] = Meta(name, int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)),
                                int(m.group(6)), int(m.group(7), 16), int(m.group(8), 16), m.group(9) == "true",
                                fields[m.group(10)])
    return _metas


def _bits(data: bytes, base: int, offset: int, size: int) -> int:
    word = int.from_bytes(data[base + (offset >> 3): base + (offset >> 3) + 8].ljust(8, b"\0"), "little")
    return (word >> (offset & 7)) & ((1 << size) - 1)


class DB2:
    def __init__(self, name: str, dbc_dir: Path = DBC_DIR):
        self.meta = meta = metas()[name]
        f = (dbc_dir / f"{name}.db2").read_bytes()
        (sig, rec_count, field_count, rec_size, str_size, table_hash, layout_hash, self.min_id, self.max_id,
         _, flags, _, total_fields, packed_off, lookup_cols, storage_size, common_size, pallet_size,
         section_count) = struct.unpack_from("<10IHH7I", f, 0)
        if sig != 0x34434457 or layout_hash != meta.layout_hash or table_hash != meta.table_hash:
            raise ValueError(f"{name}: not the 54261 layout")
        sparse = bool(flags & 1)
        pos = 0x48
        sections = []
        for _ in range(section_count):
            sections.append(struct.unpack_from("<Q8I", f, pos))
            pos += 0x28
        pos += 4 * field_count
        self.info = []
        for _ in range(storage_size // 0x18):
            self.info.append(struct.unpack_from("<HHI4I", f, pos))
            pos += 0x18
        self.pallets = [[] for _ in self.info]
        for kind in (PALLET, PALLET_ARRAY):
            for i, inf in enumerate(self.info):
                if inf[3] == kind:
                    n = inf[2] // 4
                    self.pallets[i] = list(struct.unpack_from(f"<{n}I", f, pos))
                    pos += 4 * n
        self.common = [{} for _ in self.info]
        for i, inf in enumerate(self.info):
            if inf[3] == COMMON:
                for _ in range(inf[2] // 8):
                    k, v = struct.unpack_from("<2I", f, pos)
                    self.common[i][k] = v
                    pos += 8
        for sec in sections:
            if sec[0]:
                (n,) = struct.unpack_from("<I", f, pos)
                pos += 4 + 4 * n

        self.packed_off = packed_off
        self.rec_size = rec_size
        # regular files: every section's records, then every section's strings (string offsets
        # are relative to the column across that concatenation)
        records_blob, strings_blob = bytearray(), bytearray()
        recs = []   # (id, parent, base offset in blob or file, size, in_blob)
        self.copies = []
        for (key, off, count, ssize, rec_end, idlist, rel, offmap_count, copy_count) in sections:
            first = len(recs)
            pos = off
            if not sparse:
                for i in range(count):
                    recs.append([0, 0, len(records_blob) + i * rec_size, rec_size])
                records_blob += f[pos: pos + rec_size * count]
                strings_blob += f[pos + rec_size * count: pos + rec_size * count + ssize]
                pos += rec_size * count + ssize
            else:
                pos = rec_end
            ids = list(struct.unpack_from(f"<{idlist // 4}I", f, pos))
            pos += idlist
            for _ in range(copy_count):
                self.copies.append(struct.unpack_from("<2I", f, pos))
                pos += 8
            offmap = [struct.unpack_from("<IH", f, pos + 6 * i) for i in range(offmap_count)]
            pos += 6 * offmap_count
            parents = []
            if rel:
                (n, _, _) = struct.unpack_from("<3I", f, pos)
                pos += 12
                parents = [struct.unpack_from("<2I", f, pos + 8 * i) for i in range(n)]
                pos += 8 * n
            if sparse:
                offmap_ids = list(struct.unpack_from(f"<{offmap_count}I", f, pos))
                pos += 4 * offmap_count
                if not ids:
                    ids = offmap_ids
                for o, s in offmap:
                    recs.append([0, 0, o, s])
            for i, rid in enumerate(ids[: len(recs) - first]):
                recs[first + i][0] = rid
            for parent, idx in parents:
                recs[first + idx][1] = parent
        self.data = bytes(records_blob) + bytes(strings_blob) + b"\0" * 8 if not sparse else f
        self.sparse = sparse
        self.recs = recs
        if meta.index_field != -1:
            for r in recs:
                r[0] = self._raw(r, meta.index_field, 0)
        if meta.parent_index_field != -1 and meta.parent_index_field < meta.file_field_count:
            for r in recs:
                r[1] = self._raw(r, meta.parent_index_field, 0)

    # ---- columns ---------------------------------------------------------------------------
    def _sparse_offsets(self, r):
        out, off, base = [], 0, r[2]
        for fl, fi in zip(self.meta.fields[: self.meta.file_field_count], range(self.meta.file_field_count)):
            row = []
            for _ in range(fl.array):
                row.append(off)
                if fl.type == STRING:
                    off = self.data.index(b"\0", base + off) - base + 1
                elif fl.type == FLOAT:
                    off += 4
                else:
                    off += WIDTH.get(fl.type, 4)
            out.append(row)
        return out

    def _raw(self, r, field, a, so=None):
        fl = self.meta.fields[field]
        if self.sparse:
            so = so or self._sparse_offsets(r)
            w = min(WIDTH.get(fl.type, 4), 4)
            return int.from_bytes(self.data[r[2] + so[field][a]: r[2] + so[field][a] + w], "little")
        off_bits, size_bits, _, storage, v1, v2, v3 = self.info[field]
        base = r[2]
        if storage == NONE:
            w = WIDTH.get(fl.type, 4)
            p = base + (off_bits >> 3) + a * w
            return int.from_bytes(self.data[p: p + min(w, 4)], "little")
        packed = base + self.packed_off
        if storage == PACKED:
            return _bits(self.data, packed, v1, v2)
        if storage == COMMON:
            return self.common[field].get(r[0], v1)
        if storage == PALLET:
            return self.pallets[field][_bits(self.data, packed, v1, v2)]
        if storage == PALLET_ARRAY:
            return self.pallets[field][_bits(self.data, packed, v1, v2) * v3 + a]
        if storage == PACKED_SIGNED:
            b, s = _bits(self.data, packed, v1, v2), 1 << (v2 - 1)
            return ((b ^ s) - s) & 0xFFFFFFFF
        raise ValueError(f"{self.meta.name} column {field}: storage {storage}")

    def _value(self, r, field, a, so):
        fl = self.meta.fields[field]
        if fl.type == STRING:
            if self.sparse:
                p = r[2] + so[field][a]
            else:
                col = r[2] + (self.info[field][0] >> 3) + 4 * a
                (o,) = struct.unpack_from("<I", self.data, col)
                if not o:
                    return ""
                p = col + o
            return self.data[p: self.data.index(b"\0", p)].decode("utf-8", "replace")
        if fl.type == INT64 and (self.sparse or self.info[field][3] == NONE):
            p = r[2] + (so[field][a] if self.sparse else (self.info[field][0] >> 3) + 8 * a)
            return struct.unpack_from("<q" if fl.signed else "<Q", self.data, p)[0]
        raw = self._raw(r, field, a, so)
        if fl.type == FLOAT:
            return struct.unpack("<f", struct.pack("<I", raw & 0xFFFFFFFF))[0]
        bits = {INT8: 8, INT16: 16}.get(fl.type, 32)
        raw &= (1 << bits) - 1
        if fl.signed and raw >> (bits - 1):
            raw -= 1 << bits
        return raw

    def rows(self):
        """(id, parent id, [column values, each a list of its array elements]) per record."""
        n = self.meta.file_field_count
        for r in self.recs:
            so = self._sparse_offsets(r) if self.sparse else None
            yield r[0], r[1], [[self._value(r, f, a, so) for a in range(self.meta.fields[f].array)] for f in range(n)]

    def by_id(self) -> dict[int, tuple[int, list]]:
        out = {rid: (parent, cols) for rid, parent, cols in self.rows()}
        for new, src in self.copies:
            if src in out:
                out[new] = out[src]
        return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("table")
    ap.add_argument("--dbc", type=Path, default=DBC_DIR)
    ap.add_argument("--id", type=int, action="append")
    ap.add_argument("--limit", type=int, default=10)
    args = ap.parse_args()
    db = DB2(args.table, args.dbc)
    print(f"{args.table}: {len(db.recs)} records, ids {db.min_id}..{db.max_id}, "
          f"{db.meta.file_field_count} columns: " + " ".join(f"{i}:{f.type}[{f.array}]" for i, f in enumerate(db.meta.fields)))
    shown = 0
    for rid, parent, cols in db.rows():
        if args.id and rid not in args.id:
            continue
        print(rid, f"parent={parent}" if parent else "", cols)
        shown += 1
        if shown >= args.limit:
            break


if __name__ == "__main__":
    main()
