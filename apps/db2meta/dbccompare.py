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
"""Shows one store member's 3.3.5 values beside 54261 columns, row by row, to settle what
dbcmatch.py cannot: which column a member moved to when its values changed.

    python apps/db2meta/dbccompare.py --dbc335 <dir> ChrRaces Flags 15 [16 ...] [--all] [--limit 30]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import db2read  # noqa: E402
import dbcmatch  # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dbc335", type=Path, required=True)
    ap.add_argument("table")
    ap.add_argument("member")
    ap.add_argument("columns", nargs="*", help="54261 columns as N or N[i]")
    ap.add_argument("--all", action="store_true", help="also rows where the first column agrees")
    ap.add_argument("--limit", type=int, default=30)
    args = ap.parse_args()

    def table_of(store):
        stem = store["file"].rsplit(".", 1)[0]
        return dbcmatch.RENAMED.get(stem, stem).lower()

    st = next(s for s in dbcmatch.stores() if table_of(s) == args.table.lower())
    names = dbcmatch.members(st["struct"])
    loaded = [i for i, c in enumerate(st["fmt"]) if c not in dbcmatch.SKIPPED]
    col335 = loaded[names.index(args.member)] if names and args.member in names else int(args.member.removeprefix("col"))
    old = dbcmatch.read_dbc(dbcmatch.dbc_path(args.dbc335, st["file"]), st["fmt"])
    new = db2read.DB2(args.table).by_id()
    cols = []
    for c in args.columns:
        f, _, a = c.partition("[")
        cols.append((c, int(f), int(a.rstrip("]") or 0)))
    meta = db2read.metas()[args.table]
    shown = 0
    print(f"{args.table}.{args.member} (3.3.5 column {col335}) vs " + ", ".join(c[0] for c in cols))
    for rid in sorted(set(old) & set(new)):
        v = old[rid][col335]
        vals = [new[rid][1][f][a] for _, f, a in cols]
        if not args.all and vals and dbcmatch.same(v, vals[0], st["fmt"][col335]):
            continue
        fmt = (lambda x: f"0x{x & 0xFFFFFFFF:x}") if isinstance(v, int) and abs(v) > 0xFFFF else repr
        print(f"  {rid:>7}  {fmt(v):>14}  |  " + "  ".join(f"{fmt(x):>14}" for x in vals))
        shown += 1
        if shown >= args.limit:
            break
    print(f"columns: " + " ".join(f"{i}:{f.type}[{f.array}]" for i, f in enumerate(meta.fields)))


if __name__ == "__main__":
    main()
