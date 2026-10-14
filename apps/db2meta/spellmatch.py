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
"""Finds where each member of AC's SpellEntry lives in the 3.4.3.54261 spell tables.

3.3.5 kept a spell in one Spell.dbc row; 54261 spreads it over a dozen tables, each keyed by the
spell (through its parent link or a spell id column), several with one row per difficulty, and
SpellEffect with one row per effect. This joins them into one row per spell and compares every
AC member with every joined column, as dbcmatch.py does for single tables:

  - a table's spell key is its parent link, or else the int column that holds the most 3.3.5
    spell ids with the fewest repeats;
  - where a table has several rows for a spell, the one with the lowest id is taken (the
    difficulty-0 row in every table checked);
  - SpellEffect rows are keyed by (spell, effect index); the effect index is the column whose
    values, under one spell, are 0, 1, 2 ... An AC member X[i] is compared with effect i.

Labels are Table.column or Table.column[element], with @effect for SpellEffect.

    python apps/db2meta/spellmatch.py --dbc335 <dir> [--json out.json] [--out report.md]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import db2read  # noqa: E402
import dbcmatch  # noqa: E402

TABLES = ["SpellName", "Spell", "SpellMisc", "SpellAuraOptions", "SpellAuraRestrictions", "SpellCastingRequirements",
          "SpellCategories", "SpellClassOptions", "SpellCooldowns", "SpellEquippedItems", "SpellInterrupts",
          "SpellLevels", "SpellPower", "SpellReagents", "SpellScaling", "SpellShapeshift",
          "SpellTargetRestrictions", "SpellTotems", "SpellXSpellVisual", "SpellAuraVisibility"]
EFFECT_TABLE = "SpellEffect"
# a spell without a row in a table has that table's defaults
DEFAULT = {"i": 0, "f": 0.0, "s": ""}


def spell_key(db: db2read.DB2, spells: set[int]) -> tuple[str, int | None]:
    """('id'|'parent'|'column', column) by which the table's rows name their spell."""
    rows = list(db.rows())
    by_parent = sum(1 for _, p, _ in rows if p in spells)
    by_id = sum(1 for r, _, _ in rows if r in spells)
    best = ("parent", None, by_parent)
    if by_id > best[2] and db.meta.name in ("SpellName", "Spell"):
        best = ("id", None, by_id)
    for f, fl in enumerate(db.meta.fields[: db.meta.file_field_count]):
        if fl.type in (db2read.STRING, db2read.FLOAT) or fl.array != 1:
            continue
        values = [c[f][0] for _, _, c in rows]
        hit = sum(1 for v in values if v in spells)
        if hit > best[2] * 1.2:
            best = ("column", f, hit)
    return best[0], best[1]


def rows_by_spell(db: db2read.DB2, key, extra=None) -> dict:
    out = {}
    for rid, parent, cols in sorted(db.rows(), key=lambda r: r[0]):
        spell = rid if key[0] == "id" else parent if key[0] == "parent" else cols[key[1]][0]
        k = spell if extra is None else (spell, cols[extra][0])
        out.setdefault(k, (rid, cols))
    return out


def effect_index_column(db: db2read.DB2, key) -> int:
    rows = list(db.rows())
    best, best_score = None, -1
    for f, fl in enumerate(db.meta.fields[: db.meta.file_field_count]):
        if fl.type in (db2read.STRING, db2read.FLOAT) or fl.array != 1:
            continue
        per_spell = defaultdict(set)
        for rid, parent, cols in rows:
            spell = parent if key[0] == "parent" else cols[key[1]][0]
            per_spell[spell].add(cols[f][0])
        score = sum(1 for s in per_spell.values() if s == set(range(len(s))))
        if score > best_score:
            best, best_score = f, score
    return best


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dbc335", type=Path, required=True)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()

    st = next(s for s in dbcmatch.stores() if s["store"] == "sSpellStore")
    fmt = st["fmt"]
    old = dbcmatch.read_dbc(dbcmatch.dbc_path(args.dbc335, "Spell.dbc"), fmt)
    names = dbcmatch.members(st["struct"])
    loaded = [i for i, c in enumerate(fmt) if c not in dbcmatch.SKIPPED]
    if names is None or len(names) != len(loaded):
        names = [f"col{c}" for c in loaded]
    spells = set(old)

    # every joined column: label -> {spell: value}; effect columns: label -> {(spell, index): value}
    columns: dict[str, tuple[str, dict]] = {}
    keys = {}
    for t in TABLES:
        if t not in db2read.metas():
            continue
        db = db2read.DB2(t)
        key = spell_key(db, spells)
        keys[t] = key
        rows = rows_by_spell(db, key)
        for f, fl in enumerate(db.meta.fields[: db.meta.file_field_count]):
            if key[0] == "column" and f == key[1]:
                continue
            cls = {db2read.STRING: "s", db2read.FLOAT: "f"}.get(fl.type, "i")
            for a in range(fl.array):
                label = f"{t}.{f}" + (f"[{a}]" if fl.array > 1 else "")
                columns[label] = (cls, {s: c[f][a] for s, (_, c) in rows.items()})
    eff = db2read.DB2(EFFECT_TABLE)
    ekey = spell_key(eff, spells)
    eidx = effect_index_column(eff, ekey)
    keys[EFFECT_TABLE] = (*ekey, eidx)
    erows = rows_by_spell(eff, ekey, eidx)
    effect_columns = {}
    for f, fl in enumerate(eff.meta.fields[: eff.meta.file_field_count]):
        if f == eidx or (ekey[0] == "column" and f == ekey[1]):
            continue
        cls = {db2read.STRING: "s", db2read.FLOAT: "f"}.get(fl.type, "i")
        for a in range(fl.array):
            label = f"{EFFECT_TABLE}.{f}" + (f"[{a}]" if fl.array > 1 else "")
            effect_columns[label] = (cls, {k: c[f][a] for k, (_, c) in erows.items()})

    shared = sorted(spells & ({s for s, (_, _) in columns.get("SpellMisc.0", ("", {}))[1].items()} | set()))
    shared = sorted(spells & set().union(*(set(v[1]) for v in columns.values())))
    rows = []
    for k, c in enumerate(loaded):
        kind, member = fmt[c], names[k]
        m = re.fullmatch(r"(\w+)\[(\d+)\]", member)
        vals = {s: old[s][c] for s in shared}
        distinct = len(set(vals.values()))
        if kind == "s" and distinct == 1 and set(vals.values()) <= {""}:
            rows.append({"member": member, "col335": c, "kind": kind, "distinct": distinct, "hits": [], "locale": True})
            continue
        common = Counter(vals.values()).most_common(1)[0][0] if vals else None
        informative = [s for s in shared if vals[s] != common]
        member_cls = {"s": "s", "f": "f"}.get(kind, "i")
        hits = []
        candidates = [(label, cls, (lambda s, v=v, d=DEFAULT[cls]: v.get(s, d))) for label, (cls, v) in columns.items()]
        if m:
            i = int(m.group(2))
            candidates += [(f"{label}@{i}", cls, (lambda s, v=v, i=i, d=DEFAULT[cls]: v.get((s, i), d)))
                           for label, (cls, v) in effect_columns.items()]
        for label, cls, get in candidates:
            if cls != member_cls:
                continue
            pool = informative if distinct > 1 else shared
            if not pool:
                continue
            agree = sum(1 for s in pool if (x := get(s)) is not None and dbcmatch.same(vals[s], x, kind))
            if agree / len(pool) < 0.5:
                continue
            overall = sum(1 for s in shared if (x := get(s)) is not None and dbcmatch.same(vals[s], x, kind))
            if distinct > 1 and overall / len(shared) < 0.5:
                continue
            hits.append((agree / len(pool), label))
        hits.sort(key=lambda h: -h[0])
        rows.append({"member": member, "col335": c, "kind": kind, "distinct": distinct, "hits": hits[:12]})

    result = {"store": "sSpellStore", "struct": st["struct"], "file": "Spell.dbc", "table": "Spell",
              "keys": {t: list(k) for t, k in keys.items()}, "old": len(old), "shared": len(shared), "rows": rows}
    if args.json:
        args.json.write_text(json.dumps(result, indent=1), encoding="utf-8", newline="\n")
    lines = ["# SpellEntry onto the 3.4.3.54261 spell tables", "", "Generated by `apps/db2meta/spellmatch.py`.", "",
             "Keys: " + ", ".join(f"{t} by {k[0]}{'' if k[1] is None else ' ' + str(k[1])}" for t, k in keys.items()),
             "", f"3.3.5 spells {len(old)}, in the 54261 tables {len(shared)}.", "",
             "| member | 3.3.5 col | fmt | distinct | 54261 hits |", "|---|---|---|---|---|"]
    for x in rows:
        if x.get("locale"):
            continue
        hits = ", ".join(f"{h[1]} ({h[0]:.0%})" for h in x["hits"][:4]) or "**none**"
        lines.append(f"| {x['member']} | {x['col335']} | {x['kind']} | {x['distinct']} | {hits} |")
    text = "\n".join(lines) + "\n"
    if args.out:
        args.out.write_text(text, encoding="utf-8", newline="\n")
    else:
        print(text)


if __name__ == "__main__":
    main()
