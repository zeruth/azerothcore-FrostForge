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
"""Finds where each member of AC's DBC stores lives in the 3.4.3.54261 client tables.

The 54261 tables carry no column names. AC's stores do, through their structs and format
strings over the 3.3.5 files. So for every record id the two builds share, each member AC reads
is compared with every column of the 54261 table, and the columns that agree are its source.

    python apps/db2meta/dbcmatch.py --dbc335 <3.3.5 dbc dir> [--store Map] [--out report.md]
"""

from __future__ import annotations

import argparse
import math
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import db2read  # noqa: E402

ROOT = db2read.ROOT
STORES_CPP = ROOT / "src/server/game/DataStores/DBCStores.cpp"
FMT_H = ROOT / "src/server/shared/DataStores/DBCfmt.h"
STRUCT_H = ROOT / "src/server/shared/DataStores/DBCStructure.h"
HEADERS = [STRUCT_H, ROOT / "src/server/shared/DataStores/DBCEnums.h", ROOT / "src/server/shared/SharedDefines.h"]

# format characters that are not loaded into the struct
SKIPPED = set("xXd")

# 3.3.5 tables whose records live on under another name in 54261, ids kept
RENAMED = {
    "SoundEntries": "SoundKit",     # every sound id is a SoundKit id; AC only checks they exist
    "Achievement_Criteria": "Criteria",
}


def stores() -> list[dict]:
    """[{store, struct, fmt, file}] for every LOAD_DBC in DBCStores.cpp."""
    text = STORES_CPP.read_text(encoding="utf-8")
    decl = {m.group(2): (m.group(1), m.group(3))
            for m in re.finditer(r"DBCStorage\s*<\s*(\w+)\s*>\s*(\w+)\((\w+)\)", text)}
    fmts = dict(re.findall(r"char constexpr (\w+)\[\] = \"([^\"]*)\"", FMT_H.read_text(encoding="utf-8")))
    out = []
    # a store still on its 3.3.5 file, or one already loaded from its 54261 table (the 3.3.5
    # file it was matched against is named for the table, through RENAMED)
    renamed_from = {v.lower(): k for k, v in RENAMED.items()}
    for m in re.finditer(r"LOAD_DBC\((\w+),\s*\"([\w.]+)\"|LOAD_DB2(?:_FIXUP)?\((\w+),\s*DBCSources::(\w+)Info", text):
        if m.group(1):
            st, f = m.group(1), m.group(2)
        else:
            st, f = m.group(3), renamed_from.get(m.group(4).lower(), m.group(4)) + ".dbc"
        if st in decl:
            out.append({"store": st, "struct": decl[st][0], "fmt": fmts.get(decl[st][1], ""), "file": f})
    return out


def dbc_path(directory: Path, name: str) -> Path:
    """A 3.3.5 file by name, whatever its case on disk (PvpDifficulty.dbc for PVPDifficulty)."""
    exact = directory / name
    if exact.exists():
        return exact
    return next((p for p in directory.iterdir() if p.name.lower() == name.lower()), exact)


_consts: dict[str, int] | None = None


def const(name: str) -> int | None:
    global _consts
    if _consts is None:
        _consts = {}
        for h in HEADERS:
            if h.exists():
                t = h.read_text(encoding="utf-8", errors="replace")
                for m in re.finditer(r"#define\s+(\w+)\s+(\d+)\b", t):
                    _consts[m.group(1)] = int(m.group(2))
                for m in re.finditer(r"\b(\w+)\s*=\s*(\d+)\s*[,;]", t):
                    _consts.setdefault(m.group(1), int(m.group(2)))
    if name.isdigit():
        return int(name)
    return _consts.get(name)


def members(struct_name: str) -> list[str] | None:
    """The struct's loaded members in order, one entry per element ('name[i]' for arrays)."""
    text = STRUCT_H.read_text(encoding="utf-8")
    m = re.search(r"struct " + struct_name + r"\s*\{(.*?)\n\};", text, re.S)
    if not m:
        return None
    out = []
    for line in m.group(1).splitlines():
        line = line.split("//")[0].strip()
        if not line or "(" in line or line.startswith(("[[", "#", "enum", "struct", "union", "}", "{")):
            continue
        # statements inside helper bodies
        if line.split()[0] in ("return", "if", "else", "for", "while", "case", "break", "continue", "using"):
            continue
        # flag96: three 32-bit words. In an array they are named Name<word>[element], so an
        # effect-indexed array keeps its element as the index
        fm = re.match(r"^std::array<\s*flag96\s*,\s*(\w+)\s*>\s*(\w+)\s*;$", line)
        if fm:
            n = const(fm.group(1))
            if n is None:
                return None
            out += [f"{fm.group(2)}{w}[{i}]" for i in range(n) for w in range(3)]
            continue
        fm = re.match(r"^flag96\s+(\w+)\s*;$", line)
        if fm:
            out += [f"{fm.group(1)}[{w}]" for w in range(3)]
            continue
        am = re.match(r"^std::array<\s*(?:std::array<[^<>]*>|[\w:*\s]+?)\s*,\s*(\w+)\s*>\s*(\w+)\s*;$", line)
        if am:
            # std::array<std::array<T, M>, N> holds N*M elements
            inner = re.match(r"^std::array<\s*std::array<[^,<>]*,\s*(\w+)\s*>", line)
            n, m_ = const(am.group(1)), const(inner.group(1)) if inner else 1
            if n is None or m_ is None:
                return None
            out += [f"{am.group(2)}[{i}]" for i in range(n * m_)]
            continue
        pm = re.match(r"^DBCPosition([23])D\s+(\w+)\s*;$", line)
        if pm:
            out += [f"{pm.group(2)}.{a}" for a in "XYZ"[: int(pm.group(1))]]
            continue
        mm = re.match(r"^(?:const\s+)?[\w:<>]+(?:\s+const)?\s*\**\s*(?:const\s*\*?)?\s*(\w+)\s*((?:\[\s*\w+\s*\])*)\s*;$", line)
        if not mm:
            continue
        dims = [const(d) for d in re.findall(r"\[\s*(\w+)\s*\]", mm.group(2))]
        if any(d is None for d in dims):
            return None
        n = math.prod(dims) if dims else 1
        out += [mm.group(1)] if n == 1 and not dims else [f"{mm.group(1)}[{i}]" for i in range(n)]
    return out


def read_dbc(path: Path, fmt: str) -> dict[int, list]:
    """3.3.5 WDBC records by their first column: per column a raw int (or a string for 's')."""
    d = path.read_bytes()
    sig, n, fields, size, ssize = struct.unpack_from("<4s4I", d, 0)
    strings = 20 + n * size
    widths = [1 if c in "bX" else 4 for c in fmt] if len(fmt) == fields else [4] * fields
    out = {}
    for r in range(n):
        p = 20 + r * size
        row = []
        for i, w in enumerate(widths):
            v = int.from_bytes(d[p: p + w], "little")
            p += w
            c = fmt[i] if i < len(fmt) else "i"
            if c == "s":
                e = d.index(b"\0", strings + v)
                v = d[strings + v: e].decode("utf-8", "replace")
            elif c == "f":
                v = struct.unpack("<f", struct.pack("<I", v))[0]
            row.append(v)
        out[int.from_bytes(d[20 + r * size: 24 + r * size], "little")] = row
    return out


def same(a, b, kind: str) -> bool:
    if kind == "s":
        return isinstance(b, str) and a == b
    if isinstance(b, str):
        return False
    if kind == "f" or isinstance(b, float):
        try:
            return math.isclose(float(a), float(b), rel_tol=1e-4, abs_tol=1e-4)
        except (TypeError, ValueError):
            return False
    return (int(a) & 0xFFFFFFFF) == (int(b) & 0xFFFFFFFF)


def match_store(st: dict, dbc335: Path, table: str) -> dict:
    fmt = st["fmt"]
    old = read_dbc(dbc_path(dbc335, st["file"]), fmt)
    new = db2read.DB2(table).by_id()
    shared = sorted(set(old) & set(new))
    names = members(st["struct"])
    loaded = [i for i, c in enumerate(fmt) if c not in SKIPPED]
    if names is not None and len(names) != len(loaded):
        names = None
    meta = db2read.metas()[table]
    # 54261 columns as (label, values by id)
    # (label, type class, values by id); a member only matches a column of its own class
    cols = [("id", "i", {i: i for i in shared}), ("parent", "i", {i: new[i][0] for i in shared})]
    for f in range(meta.file_field_count):
        cls = {db2read.STRING: "s", db2read.FLOAT: "f"}.get(meta.fields[f].type, "i")
        for a in range(meta.fields[f].array):
            label = f"{f}" if meta.fields[f].array == 1 else f"{f}[{a}]"
            cols.append((label, cls, {i: new[i][1][f][a] for i in shared}))
    rows = []
    for k, c in enumerate(loaded):
        kind = fmt[c]
        member = names[k] if names else f"col{c}"
        vals = {i: old[i][c] for i in shared}
        distinct = len(set(vals.values()))
        # agreement is measured on the rows that carry information: where the member is not its
        # most common value (a mostly-zero member otherwise "agrees" with every mostly-zero column)
        common = max(set(vals.values()), key=list(vals.values()).count) if vals else None
        informative = [i for i in shared if vals[i] != common]
        # the other locales of a localized string: empty in an enUS install, and a 54261 table
        # file holds one locale only
        if kind == "s" and distinct == 1 and set(vals.values()) <= {""}:
            rows.append({"member": member, "col335": c, "kind": kind, "distinct": distinct, "hits": [],
                         "locale": True})
            continue
        hits = []
        member_cls = {"s": "s", "f": "f"}.get(kind, "i")
        for label, cls, nv in cols:
            if cls != member_cls:
                continue
            rows_ = informative if distinct > 1 else shared
            m = sum(1 for i in rows_ if same(vals[i], nv[i], kind))
            if rows_ and m / len(rows_) >= 0.5:
                # and the common value must carry over too, or a column that is the member plus noise
                # elsewhere would pass
                if distinct > 1 and sum(1 for i in shared if same(vals[i], nv[i], kind)) / len(shared) < 0.5:
                    continue
                hits.append((m / len(rows_), label))
        hits.sort(key=lambda h: -h[0])
        rows.append({"member": member, "col335": c, "kind": kind, "distinct": distinct, "hits": hits})
    return {"store": st["store"], "struct": st["struct"], "file": st["file"], "table": table,
            "old": len(old), "new": len(new), "shared": len(shared), "rows": rows}


def render(results: list[dict], missing: list[dict]) -> str:
    out = ["# FrostForge data map: AC's DBC stores onto 3.4.3.54261 tables", "",
           "Generated by `apps/db2meta/dbcmatch.py`: every member AC loads, compared with every column",
           "of the same-named 54261 table on the record ids both builds share. Agreement is measured on",
           "the rows where the member is not its most common value, so a mostly-zero member does not",
           "match every mostly-zero column; a hit agrees on at least half of those rows (and on half of",
           "all rows), shown as `column (agreement)`. A member whose 3.3.5 values are all alike",
           "(`distinct` 1) matches any constant column, so its hits prove nothing.", ""]
    out += ["## Summary", "", "| store | 3.3.5 file | 54261 table | rows 3.3.5 / 54261 / shared | members placed |",
            "|---|---|---|---|---|"]
    for r in results:
        placed = sum(1 for x in r["rows"] if x["hits"] and x["hits"][0][0] >= 0.9 and x["distinct"] > 1)
        out.append(f"| {r['store']} | {r['file']} | {r['table']} | {r['old']} / {r['new']} / {r['shared']} | "
                   f"{placed} / {len(r['rows'])} |")
    for m in missing:
        out.append(f"| {m['store']} | {m['file']} | **none** | | |")
    for r in results:
        out += ["", f"## {r['struct']} ({r['file']} -> {r['table']})", "",
                "| member | 3.3.5 col | fmt | distinct | 54261 hits |", "|---|---|---|---|---|"]
        for x in r["rows"]:
            if x.get("locale"):
                continue
            hits =", ".join(f"{h[1]} ({h[0]:.0%})" for h in x["hits"][:4]) or "**none**"
            out.append(f"| {x['member']} | {x['col335']} | {x['kind']} | {x['distinct']} | {hits} |")
    return "\n".join(out) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dbc335", type=Path, required=True)
    ap.add_argument("--store", action="append")
    ap.add_argument("--out", type=Path)
    ap.add_argument("--json", type=Path, help="also write the raw results, for dbcsources.py")
    args = ap.parse_args()
    tables = db2read.metas()
    results, missing = [], []
    for st in stores():
        if args.store and st["store"] not in args.store and st["file"].split(".")[0] not in args.store:
            continue
        table = st["file"].rsplit(".", 1)[0]
        # names whose case changed (PvpDifficulty -> PVPDifficulty), and tables renamed
        table = RENAMED.get(table, table)
        table = next((t for t in tables if t.lower() == table.lower()), table)
        if table not in tables or not (db2read.DBC_DIR / f"{table}.db2").exists():
            missing.append(st)
            continue
        try:
            results.append(match_store(st, args.dbc335, table))
        except Exception as e:  # report and go on: one bad table should not hide the rest
            print(f"{st['store']}: {e}", file=sys.stderr)
            missing.append(st)
    if args.json:
        import json
        args.json.write_text(json.dumps({"results": results, "missing": missing}, indent=1), encoding="utf-8",
                             newline="\n")
    text = render(results, missing)
    if args.out:
        args.out.write_text(text, encoding="utf-8", newline="\n")
    else:
        print(text)


if __name__ == "__main__":
    main()
