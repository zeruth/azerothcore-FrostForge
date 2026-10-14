"""ID stability: the world DB rows that reference a client record 54261 no longer has.

For each kind of client record the world DB points at (spells, items, displays, factions, maps,
areas, skills), the ids of the 3.3.5 file and of the 54261 table are compared, and every
referencing column of the world DB is checked against both:

- "dropped": the id is in 3.3.5 but not in 54261. These rows break on 54261 data.
- "unknown": the id is in neither. Server-side records (spell_dbc) or rows already broken on 3.3.5.

Spells shared by both builds are also checked by name (3.3.5 Spell.dbc enUS name vs SpellName),
since an id kept under another name is a repurposed record.

The world DB is read from data/sql/base/db_world (mysqldump files), not a live server, so pending
updates are not included.

    python apps/db2meta/idstable.py --dbc335 <3.3.5 dbc dir> [--out id-stability.ANALYSIS.md]
"""
import argparse
import collections
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import db2read  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
WORLD = ROOT / "data/sql/base/db_world"

# kind: (3.3.5 file, 54261 table)
KINDS = {
    "spell": ("Spell", "SpellName"),
    "item": ("Item", "Item"),
    "creature display": ("CreatureDisplayInfo", "CreatureDisplayInfo"),
    "gameobject display": ("GameObjectDisplayInfo", "GameObjectDisplayInfo"),
    "item display": ("ItemDisplayInfo", "ItemDisplayInfo"),
    "faction template": ("FactionTemplate", "FactionTemplate"),
    "faction": ("Faction", "Faction"),
    "map": ("Map", "Map"),
    "area": ("AreaTable", "AreaTable"),
    "skill": ("SkillLine", "SkillLine"),
}

# (table, column, kind); a value of 0 means no reference, a negative one is taken as its magnitude
REFS = [
    ("creature_template_spell", "Spell", "spell"),
    ("npc_trainer", "SpellID", "spell"),
    ("npc_trainer", "ReqSkillLine", "skill"),
    ("spell_area", "spell", "spell"),
    ("spell_area", "area", "area"),
    ("npc_spellclick_spells", "spell_id", "spell"),
    ("spell_linked_spell", "spell_trigger", "spell"),
    ("spell_linked_spell", "spell_effect", "spell"),
    ("playercreateinfo_spell_custom", "Spell", "spell"),
    ("spell_proc", "SpellId", "spell"),
    ("spell_bonus_data", "entry", "spell"),
    ("spell_ranks", "first_spell_id", "spell"),
    ("spell_ranks", "spell_id", "spell"),
    ("item_template", "entry", "item"),
    ("item_template", "spellid_1", "spell"),
    ("item_template", "spellid_2", "spell"),
    ("item_template", "spellid_3", "spell"),
    ("item_template", "spellid_4", "spell"),
    ("item_template", "spellid_5", "spell"),
    ("item_template", "displayid", "item display"),
    ("item_template", "RequiredSkill", "skill"),
    ("item_template", "RequiredReputationFaction", "faction"),
    ("npc_vendor", "item", "item"),
    ("playercreateinfo_item", "itemid", "item"),
    ("quest_template", "RewardItem1", "item"),
    ("quest_template", "RewardItem2", "item"),
    ("quest_template", "RewardItem3", "item"),
    ("quest_template", "RewardItem4", "item"),
    ("quest_template", "RewardSpell", "spell"),
    ("quest_template", "RewardFactionID1", "faction"),
    ("quest_template", "RewardFactionID2", "faction"),
    ("quest_template", "RewardFactionID3", "faction"),
    ("quest_template", "RewardFactionID4", "faction"),
    ("quest_template", "RewardFactionID5", "faction"),
    ("creature_template_model", "CreatureDisplayID", "creature display"),
    ("creature_template", "faction", "faction template"),
    ("creature_onkill_reputation", "RewOnKillRepFaction1", "faction"),
    ("creature_onkill_reputation", "RewOnKillRepFaction2", "faction"),
    ("gameobject_template", "displayId", "gameobject display"),
    ("gameobject_template_addon", "faction", "faction template"),
    ("creature", "map", "map"),
    ("creature", "zoneId", "area"),
    ("creature", "areaId", "area"),
    ("gameobject", "map", "map"),
    ("gameobject", "zoneId", "area"),
    ("gameobject", "areaId", "area"),
    ("instance_template", "map", "map"),
    ("areatrigger_teleport", "target_map", "map"),
    ("game_tele", "map", "map"),
    ("playercreateinfo", "map", "map"),
    ("playercreateinfo", "zone", "area"),
]

SPELL_NAME_335 = 136  # Spell.dbc: the enUS name's column


def read_wdbc(path: Path, column: int | None = None):
    """ids of a 3.3.5 WDBC file, or {id: string} for one string column."""
    data = path.read_bytes()
    _, count, fields, size, _ = struct.unpack_from("<4s4I", data)
    strings = 20 + count * size
    out = {} if column is not None else set()
    for i in range(count):
        base = 20 + i * size
        rid = struct.unpack_from("<I", data, base)[0]
        if column is None:
            out.add(rid)
        else:
            off = struct.unpack_from("<I", data, base + column * 4)[0]
            end = data.index(b"\0", strings + off)
            out[rid] = data[strings + off:end].decode("utf-8", "replace")
    return out


def dbc_file(directory: Path, name: str) -> Path:
    for p in directory.iterdir():
        if p.name.lower() == f"{name.lower()}.dbc":
            return p
    raise FileNotFoundError(name)


def ids_54261(name: str) -> set[int]:
    db = db2read.DB2(name)
    ids = {r[0] for r in db.recs}
    ids.update(new for new, src in db.copies if src in ids)
    return ids


def columns(sql: str, table: str) -> list[str]:
    m = re.search(r"CREATE TABLE `%s` \((.*?)\n\) ENGINE" % re.escape(table), sql, re.S)
    if not m:
        return []
    return re.findall(r"^\s*`([^`]+)`", m.group(1), re.M)


TOKEN = re.compile(r"'(?:[^'\\]|\\.)*'|NULL|-?[0-9.eE+-]+")


def rows(sql: str, table: str):
    """Each row of a mysqldump INSERT as raw tokens (strings stay quoted)."""
    for m in re.finditer(r"INSERT INTO `%s` VALUES\s*(.*?);\s*$" % re.escape(table), sql, re.S | re.M):
        body = m.group(1)
        i, n = 0, len(body)
        while i < n:
            if body[i] != "(":
                i += 1
                continue
            i += 1
            row = []
            while True:
                t = TOKEN.match(body, i)
                if not t:
                    raise ValueError(f"{table}: cannot read at {body[i:i + 40]!r}")
                row.append(t.group(0))
                i = t.end()
                if body[i] == ",":
                    i += 1
                elif body[i] == ")":
                    i += 1
                    break
            yield row


def value(table: str, token: str) -> int | None:
    """The record id a column token references, or None. A negative npc_trainer SpellID names
    another trainer's list, not a spell; elsewhere a negative id is the same record."""
    if token == "NULL" or token.startswith("'"):
        return None
    v = int(float(token))
    if v < 0 and table == "npc_trainer":
        return None
    return abs(v) or None


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dbc335", type=Path, required=True)
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()

    old, new = {}, {}
    for kind, (f335, t54) in KINDS.items():
        old[kind] = read_wdbc(dbc_file(args.dbc335, f335))
        new[kind] = ids_54261(t54)
    # server-side spells live in spell_dbc; they were never in the client
    sql = (WORLD / "spell_dbc.sql").read_text(encoding="utf-8")
    server_spells = {int(r[0]) for r in rows(sql, "spell_dbc")}

    results = []
    cache = {}
    for table, column, kind in REFS:
        if table not in cache:
            path = WORLD / f"{table}.sql"
            cache[table] = path.read_text(encoding="utf-8") if path.exists() else ""
        sql = cache[table]
        cols = columns(sql, table)
        if column not in cols:
            print(f"warning: {table}.{column} not found ({', '.join(cols[:12])}...)", file=sys.stderr)
            continue
        k = cols.index(column)
        total = 0
        dropped, unknown = collections.Counter(), collections.Counter()
        for row in rows(sql, table):
            v = value(table, row[k])
            if not v:
                continue
            total += 1
            if v in new[kind]:
                continue
            if v in old[kind]:
                dropped[v] += 1
            elif not (kind == "spell" and v in server_spells):
                unknown[v] += 1
        results.append((table, column, kind, total, dropped, unknown))

    names_old = read_wdbc(dbc_file(args.dbc335, "Spell"), SPELL_NAME_335)
    names_new = {rid: cols[0][0] for rid, (_, cols) in db2read.DB2("SpellName").by_id().items()}
    renamed = sorted(i for i in names_old.keys() & names_new.keys()
                     if names_old[i].strip().lower() != (names_new[i] or "").strip().lower())

    out = ["# ID stability: world DB references on 54261 data", "",
           "Generated by `apps/db2meta/idstable.py` from `data/sql/base/db_world` (pending updates not",
           "included). *dropped*: the id is in the 3.3.5 file but not the 54261 table, so the row breaks",
           "on 54261 data. *unknown*: in neither (server-side spells in spell_dbc are not counted).", "",
           "## Records per kind", "", "| kind | 3.3.5 | 54261 | shared | only 3.3.5 | only 54261 |",
           "|---|---|---|---|---|---|"]
    for kind in KINDS:
        a, b = old[kind], new[kind]
        out.append(f"| {kind} | {len(a)} | {len(b)} | {len(a & b)} | {len(a - b)} | {len(b - a)} |")
    out += ["", "## References", "",
            "| table.column | kind | rows | rows dropped (ids) | rows unknown (ids) | dropped ids, most used |",
            "|---|---|---|---|---|---|"]
    for table, column, kind, total, dropped, unknown in results:
        top = ", ".join(f"{i} ({c})" for i, c in dropped.most_common(6))
        out.append(f"| {table}.{column} | {kind} | {total} | {sum(dropped.values())} ({len(dropped)}) | "
                   f"{sum(unknown.values())} ({len(unknown)}) | {top} |")
    out += ["", "## Spells kept under another name", "",
            f"{len(renamed)} of {len(names_old.keys() & names_new.keys())} shared spell ids have a different "
            "enUS name. Those the world DB references:", ""]
    used = collections.Counter()
    for table, column, kind, *_ in results:
        if kind != "spell":
            continue
        k = columns(cache[table], table).index(column)
        for row in rows(cache[table], table):
            v = value(table, row[k])
            if v:
                used[v] += 1
    hit = [i for i in renamed if used[i]]
    out.append(f"{len(hit)} ids, {sum(used[i] for i in hit)} rows.")
    out += ["", "| id | 3.3.5 | 54261 | rows |", "|---|---|---|---|"]
    for i in sorted(hit, key=lambda i: -used[i])[:40]:
        out.append(f"| {i} | {names_old[i]} | {names_new[i]} | {used[i]} |")
    text = "\n".join(out) + "\n"
    if args.out:
        args.out.write_text(text, encoding="utf-8", newline="\n")
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
