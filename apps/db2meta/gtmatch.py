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
"""Lays AC's 3.3.5 gt stores (flat float arrays) over the 54261 GameTables (a row per level or
index, a column per class or category), to find how the client's table is indexed.

The 54261 files are extracted to var/data/gt/GameTables (FileDataIDs from the client's own table
list, GameTablesClient/tables.cpp; var/data/extract/gt-list.txt). For each store this tries the
table read column by column (column-major: entry = column * rows + row) and row by row, over all
columns or skipping the first, and reports how much of the 3.3.5 array each layout reproduces.

    python apps/db2meta/gtmatch.py --dbc335 <dir>
"""

from __future__ import annotations

import argparse
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GT = ROOT / "var/data/gt/GameTables"

STORES = {   # 3.3.5 file -> 54261 GameTable
    "gtBarberShopCostBase.dbc": "BarberShopCostBase",
    "gtCombatRatings.dbc": "CombatRatings",
    "gtChanceToMeleeCritBase.dbc": "ChanceToMeleeCritBase",
    "gtChanceToMeleeCrit.dbc": "ChanceToMeleeCrit",
    "gtChanceToSpellCritBase.dbc": "ChanceToSpellCritBase",
    "gtChanceToSpellCrit.dbc": "ChanceToSpellCrit",
    "gtNPCManaCostScaler.dbc": "NPCManaCostScaler",
    "gtOCTClassCombatRatingScalar.dbc": "OCTClassCombatRatingScalar",
    "gtOCTRegenHP.dbc": "OCTRegenHP",
    "gtOCTRegenMP.dbc": "OCTRegenMP",
    "gtRegenHPPerSpt.dbc": "RegenHPPerSpt",
    "gtRegenMPPerSpt.dbc": "RegenMPPerSpt",
    "TeamContributionPoints.dbc": "TeamContributionPoints",
}


def read_gt_dbc(path: Path) -> list[float]:
    d = path.read_bytes()
    _, n, fields, size, _ = struct.unpack_from("<4s4I", d, 0)
    # the value is the last float of each record (some files carry an id first)
    return [struct.unpack_from("<f", d, 20 + r * size + size - 4)[0] for r in range(n)]


def read_table(path: Path) -> tuple[list[str], list[list[float]]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    header = lines[0].split("\t")
    rows = [[float(x) for x in line.split("\t")] for line in lines[1:] if line.strip()]
    return header, rows


def close(a: float, b: float) -> bool:
    return math.isclose(a, b, rel_tol=1e-3, abs_tol=1e-6)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dbc335", type=Path, required=True)
    args = ap.parse_args()
    for f, table in STORES.items():
        p = args.dbc335 / f
        if not p.exists():
            print(f"{f}: no 3.3.5 file")
            continue
        old = read_gt_dbc(p)
        header, rows = read_table(GT / f"{table}.txt")
        cols = len(header) - 1                      # the first column is the row label
        layouts = {
            "column-major": [rows[r][1 + c] for c in range(cols) for r in range(len(rows))],
            "row-major": [rows[r][1 + c] for r in range(len(rows)) for c in range(cols)],
        }
        print(f"{f}: 3.3.5 {len(old)} values; {table}: {len(rows)} rows x {cols} columns ({', '.join(header[:4])}, ...)")
        for name, flat in layouts.items():
            n = min(len(old), len(flat))
            agree = sum(1 for i in range(n) if close(old[i], flat[i]))
            print(f"    {name:13} agrees on {agree} of {n}")


if __name__ == "__main__":
    main()
