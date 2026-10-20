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
"""Generates the server's 3.4.3 update-field tables from the FrostLite client's descriptors.

3.4.3.54261 uses the changes-mask update-field system: every object type is a set of data structs
(ObjectData, UnitData, PlayerData, ...), each sent whole in a create (in declaration order,
filtered by the receiver's visibility flags) and as a change mask plus the changed values in an
update. FrostLite extracts each struct's create and update read sequences from the client's own
readers (tools/updatefields.py -> docs/updatefields.json): widths, counts, visibility flags, mask
bits and the exact wire order.

This script turns that file into src/server/game/Entities/Object/Updates/UpdateFieldsData.h:

  * a byte image layout per struct: every field at the client's own offset, so a struct's values
    are one flat buffer the server fills and diffs (UpdateFieldsWriter.h);
  * named field descriptors (offset, stride, count, width) to fill it with;
  * a create program and an update program per struct, run by UF::WriteCreate / UF::WriteUpdate.

Update entries the server cannot produce yet (dynamic arrays' element masks, nested structs with
bit-packed values) are kept in the program as Unsupported: their mask bits are never set, so the
stream stays well-formed and those values reach the client only through creates.

    python apps/updatefields/updatefields.py <FrostLite>/docs/updatefields.json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "src/server/game/Entities/Object/Updates/UpdateFieldsData.h"

WIDTH = {"u8": 1, "u16": 2, "u32": 4, "u64": 8, "guid": 16}
VIS = {"Owner": 0x01, "PartyMember": 0x02, "UnitAll": 0x04, "Empath": 0x08}

# struct name -> (C++ prefix, client type-mask bit)
STRUCT_ORDER = ["ObjectData", "ItemData", "ContainerData", "UnitData", "PlayerData", "ActivePlayerData",
                "GameObjectData", "DynamicObjectData", "CorpseData", "AreaTriggerData", "SceneObjectData",
                "ConversationData"]


def hx(v):
    return int(v, 16) if isinstance(v, str) else v


def ident(name: str | None, off) -> str:
    """A C++ identifier for a field from the client trace's descriptive name."""
    if not name:
        return "Field_%X" % off if off is not None else "Unnamed"
    n = name
    if " / " in n:                       # "Field_B0 / CurrentBattlePetSpeciesID": the inferred name
        n = n.split(" / ")[-1]
    n = re.sub(r"\s*\(.*?\)", "", n)     # remarks in parentheses
    n = n.replace("?", "").strip()
    n = re.sub(r"\[\d+\]", "", n)       # element index (arrays are merged by the caller)
    n = n.replace(".", "_")
    n = re.sub(r"[^A-Za-z0-9_]+", "_", n).strip("_")
    if not n or n[0].isdigit():
        n = "Field_%X" % off if off is not None else "Unnamed"
    return n


class Field:
    def __init__(self, name, off, width, count=1, stride=0, inner_count=1, inner_stride=0, is_float=False):
        self.name, self.off, self.width = name, off, width
        self.count, self.stride = count, stride
        self.inner_count, self.inner_stride = inner_count, inner_stride
        self.is_float = is_float

    def end(self):
        last = self.off + max(self.count - 1, 0) * self.stride + max(self.inner_count - 1, 0) * self.inner_stride
        return last + self.width


class Gen:
    def __init__(self, doc):
        self.doc = doc
        self.out = []
        self.notes = []

    # ---- create -----------------------------------------------------------------------------
    def value_instr(self, op, e, vis=None, extra=None):
        width = WIDTH[e["kind"]]
        off = hx(e["offset"]) if e.get("offset") is not None else 0
        count = e.get("count", 1)
        stride = hx(e.get("stride", 0)) if e.get("stride") else 0
        v = 0
        for f in (vis if vis is not None else e.get("visibility", [])):
            v |= VIS[f]
        d = dict(op=op, width=width, vis=v, off=off, count=count, stride=stride)
        if extra:
            d.update(extra)
        return d

    def create_program(self, sname, s):
        prog, dyns, follow = [], [], {}
        event = 0
        for e in s["create"]:
            ev_start = event
            if e.get("dynamic"):
                dyn = self.dyn_info(sname, e)
                dyns.append(dyn)
                prog.append(dict(op="DynSize", dyn=len(dyns) - 1))
                fe = e["dynamic"].get("elements_follow_event")
                follow.setdefault(fe, []).append(len(dyns) - 1)
                event += 1
            elif e["kind"] == "struct_array":
                members = e["members"]
                prog.append(dict(op="Group", count=e["count"], stride=hx(e["stride"]), members=len(members)))
                for m in members:
                    prog.append(self.value_instr("Value", m))
                event += e["count"] * sum(m.get("count", 1) for m in members)
            elif e.get("offset") is None:
                prog.append(dict(op="Zero", width=WIDTH[e["kind"]], vis=0))
                event += 1
            else:
                prog.append(self.value_instr("Value", e))
                event += e.get("count", 1)
            # dynamic elements read after one of this entry's events
            for fe in sorted(k for k in follow if k is not None and ev_start <= k < event):
                for d in follow.pop(fe):
                    prog.append(dict(op="DynElems", dyn=d))
        leftover = [d for k, ds in follow.items() for d in ds]
        if leftover:
            # follows an event past the end (or unknown): after everything
            for d in leftover:
                prog.append(dict(op="DynElems", dyn=d))
        expect = s.get("create_reads_all_flags")
        if expect is not None and expect != event:
            sys.exit("%s: create counts %d reads, the client trace %d" % (sname, event, expect))
        return prog, dyns

    def dyn_info(self, sname, e):
        elems = e["dynamic"]["element"]
        members, sources = [], []
        size = 0
        for m in elems:
            if m["kind"] not in WIDTH:
                continue
            members.append(self.value_instr("Value", m, vis=[]))
            sources.append(m)
            size = max(size, hx(m["offset"]) + WIDTH[m["kind"]] * m.get("count", 1) * 1)
            if m.get("count", 1) > 1:
                size = max(size, hx(m["offset"]) + hx(m["stride"]) * m["count"])
        size = (size + 7) & ~7 if size > 4 else size
        return dict(name=ident(e["name"], None), elem_size=size, members=members, sources=sources)

    # ---- update -----------------------------------------------------------------------------
    @staticmethod
    def int_bits(lst):
        return [b for b in lst if isinstance(b, int)]

    @staticmethod
    def elem_bit(b):
        return int(b.split(".")[1]) if isinstance(b, str) and b.startswith("elem.") else None

    def update_program(self, sname, s, vis_by_off):
        prog, parents, unsupported = [], {}, []
        entries = s["update"]
        i = 0

        def add_parents(bits, plist):
            ints = [b for b in self.int_bits(plist) if b not in bits]
            if not ints:
                return
            p = max(ints)
            for b in bits:
                if b in parents and parents[b] != p:
                    self.notes.append("%s bit %d: parents %d and %d" % (sname, b, parents[b], p))
                parents[b] = p

        def vis(off):
            return vis_by_off.get(off, 0)

        # the struct's own change mask comes first: skip it (written from the mask layout). A
        # blocks mask is the block mask (split in two reads past 32 bits) and one word per block.
        mask = s["update_mask"]
        if mask["layout"] == "single":
            header = 1
        else:
            header = mask["block_mask_reads"] + int(mask["bits"].split()[0])
        for k in range(header):
            assert entries[k].get("role") == "change mask" and entries[k]["kind"] in ("bits", "mask"), (sname, k)
        i = header
        while i < len(entries):
            e = entries[i]
            bits = self.int_bits(e.get("bits", []))
            if sname == "ActivePlayerData" and e.get("offset") is None and e["kind"] == "u32" and bits == [32]:
                # SkillInfo, a nested struct with its own 1793-bit mask (u32 + 25 bits of block mask,
                # then one 32-bit word per set block), then per skill i: 7 u16 gated by 1 + i + 256 * m.
                # The client trace stops at the mask (it served the u32 as 0); TrinityCore's layout.
                skill = [x for x in s["create"] if x.get("kind") == "struct_array" and x.get("offset") == "0x6c"][0]
                prog.append(dict(op="Skill", bit=32, count=256, stride=2, members=len(skill["members"])))
                for k, m in enumerate(skill["members"]):
                    prog.append(dict(op="Member", width=2, off=hx(m["offset"]), count=1, stride=0,
                                     bit=1 + 256 * k, vis=0))
                add_parents([32], e["parent_bits"])
                i += 1
                while i < len(entries) and entries[i].get("role") == "change mask" and \
                        (32 in entries[i].get("bits", []) or any(self.elem_bit(b) is not None for b in entries[i].get("bits", []))):
                    i += 1
                continue
            if e.get("dynamic"):
                prog.append(dict(op="Unsupported", bit=bits[0] if bits else 0xFFFF, what=e["name"]))
                unsupported.extend(bits)
                i += 1
                continue
            if e["kind"] == "struct_array":
                members = e["members"]
                if members and members[0].get("role") == "change mask":
                    mask = [m for m in members if m.get("role") == "change mask"]
                    vals = [m for m in members if m.get("role") != "change mask"]
                    ebits = self.int_bits(mask[0]["bits"])
                    if any(m["kind"] not in WIDTH for m in vals) or mask[0].get("name"):
                        prog.append(dict(op="Unsupported", bit=ebits[0], what=e.get("name") or "nested"))
                        unsupported.extend(ebits)
                        i += 1
                        continue
                    form = self.mask_form(mask)
                    prog.append(dict(op="Nested", count=e["count"], stride=hx(e["stride"]), bit=ebits[0],
                                     members=len(vals), mask=form))
                    for m in vals:
                        eb = self.elem_bit(m["bits"][0])
                        prog.append(dict(op="Member", width=WIDTH[m["kind"]], off=hx(m["offset"]),
                                         count=m.get("count", 1), stride=hx(m.get("stride", 0)) if m.get("stride") else 0,
                                         bit=eb, vis=vis(hx(m["offset"]))))
                    add_parents(ebits, mask[0]["parent_bits"])
                else:
                    prog.append(dict(op="Group", count=e["count"], stride=hx(e["stride"]), members=len(members)))
                    for m in members:
                        mb = self.int_bits(m["bits"])
                        assert mb == list(range(mb[0], mb[0] + e["count"])), (sname, m)
                        prog.append(dict(op="Member", width=WIDTH[m["kind"]], off=hx(m["offset"]), count=1,
                                         stride=0, bit=mb[0], vis=vis(hx(m["offset"]))))
                        add_parents(mb, m["parent_bits"])
                i += 1
                continue
            if e.get("role") == "change mask":
                # a single nested element listed flat (RestInfo[n]): its mask, then members on elem.k
                vals = []
                j = i + 1
                while j < len(entries) and entries[j].get("kind") in WIDTH and \
                        self.elem_bit((entries[j].get("bits") or [None])[0]) is not None:
                    vals.append(entries[j])
                    j += 1
                if vals and not e.get("name") and e.get("offset") is None:
                    base = hx(vals[0]["offset"])
                    prog.append(dict(op="Nested", count=1, stride=0, bit=bits[0], members=len(vals),
                                     mask=self.mask_form([e])))
                    for m in vals:
                        prog.append(dict(op="Member", width=WIDTH[m["kind"]], off=hx(m["offset"]), count=1,
                                         stride=0, bit=self.elem_bit(m["bits"][0]), vis=vis(hx(m["offset"]))))
                    add_parents(bits, e["parent_bits"])
                    i = j
                    continue
                prog.append(dict(op="Unsupported", bit=bits[0], what=e.get("name") or "change mask"))
                unsupported.extend(bits)
                i += 1
                # an element-masked tail that belongs to it (Field_1230's dynamic array)
                while i < len(entries) and any(self.elem_bit(b) is not None for b in entries[i].get("bits", [])):
                    i += 1
                continue
            if e["kind"] not in WIDTH:
                sys.exit("%s: unhandled update entry %r" % (sname, e))
            if e.get("offset") is None:
                prog.append(dict(op="Zero", width=WIDTH[e["kind"]], bit=bits[0], vis=0))
                add_parents(bits, e["parent_bits"])
                i += 1
                continue
            count = e.get("count", 1)
            if count > 1:
                assert bits == list(range(bits[0], bits[0] + count)), (sname, e)
            else:
                assert len(bits) == 1, (sname, e)
            off = hx(e["offset"])
            prog.append(dict(op="Value", width=WIDTH[e["kind"]], off=off, count=count,
                             stride=hx(e["stride"]) if e.get("stride") else 0, bit=bits[0], vis=vis(off),
                             each=1 if count > 1 else 0))
            add_parents(bits, e["parent_bits"])
            i += 1
        return prog, parents, unsupported

    @staticmethod
    def mask_form(mask_entries):
        """Element change mask: one word of N bits, or (bits1 + bits32) a block mask and one block."""
        ns = [int(m["type"][4:]) for m in mask_entries]
        if len(ns) == 1:
            return ns[0]
        if ns[0] == 1 and all(n == 32 for n in ns[1:]):
            return 0x8000 | (len(ns) - 1)
        raise SystemExit("unhandled element mask %r" % ns)

    # ---- fields -----------------------------------------------------------------------------
    def fields(self, sname, s):
        out = {}
        order = []

        def add(f):
            if f.name in out:
                o = out[f.name]
                if o.off == f.off:
                    return
                # "X[0]", "X[1]"...: an array listed element by element
                if o.count >= 1 and o.width == f.width and o.inner_count == 1:
                    stride = f.off - o.off if o.count == 1 else o.stride
                    if f.off == o.off + stride * o.count:
                        o.count += 1
                        o.stride = stride
                        return
                f.name = "%s_%X" % (f.name, f.off)
            out[f.name] = f
            order.append(f.name)

        for e in s["create"]:
            if e.get("dynamic") or (e["kind"] != "struct_array" and e.get("offset") is None):
                continue
            if e["kind"] == "struct_array":
                # members of a named array of structs (VisibleItems, QuestLog...) carry its name
                group = e.get("name") or ""
                prefix = "" if not group or group.startswith("(") or group.startswith("Skill") else ident(group, None) + "_"
                for m in e["members"]:
                    add(Field(prefix + ident(m["name"], hx(m["offset"])), hx(m["offset"]), WIDTH[m["kind"]], e["count"],
                              hx(e["stride"]), m.get("count", 1), hx(m.get("stride", 0)) if m.get("stride") else 0,
                              m.get("type") == "float"))
            else:
                add(Field(ident(e["name"], hx(e["offset"])), hx(e["offset"]), WIDTH[e["kind"]], e.get("count", 1),
                          hx(e["stride"]) if e.get("stride") else 0, is_float=e.get("type") == "float"))
        return [out[n] for n in order]

    # ---- emit -------------------------------------------------------------------------------
    def emit_instr(self, d):
        return "{ UFOp::%s, %d, 0x%02X, %d, 0x%04X, %d, 0x%04X, %d, 0x%04X }" % (
            d["op"], d.get("width", 0), d.get("vis", 0), d.get("members", d.get("dyn", d.get("each", 0))),
            d.get("off", 0), d.get("count", 0), d.get("stride", 0),
            d["bit"] if d.get("bit") is not None else 0xFFFF, d.get("mask", 0))

    def run(self):
        w = self.out.append
        structs = self.doc["structs"]
        w(HEADER)
        for sname in STRUCT_ORDER:
            s = structs[sname]
            short = sname[:-4]
            fields = self.fields(sname, s)
            vis_by_off = {}
            for e in s["create"]:
                es = e["members"] if e["kind"] == "struct_array" else [e]
                for m in es:
                    if m.get("offset") is not None and not m.get("dynamic"):
                        v = 0
                        for f in m.get("visibility", []):
                            v |= VIS[f]
                        vis_by_off[hx(m["offset"])] = v
            create, dyns = self.create_program(sname, s)
            update, parents, unsupported = self.update_program(sname, s, vis_by_off)
            mask = s["update_mask"]
            if mask["layout"] == "single":
                nbits, blocks = mask["bits"], 0
            else:
                blocks = int(mask["bits"].split()[0])
                nbits = blocks * 32
            size = max([f.end() for f in fields] + [8])
            size = (size + 15) & ~15
            w("    // ---- %s: type-mask bit %s, create %s, update %s ----" % (
                sname, s["type_mask_bit"], s["create_reader"], s["update_reader"]))
            w("    namespace %sFields" % short)
            w("    {")
            for f in fields:
                w("        inline constexpr UFField %s{ 0x%04X, %d, %d, 0x%X, %d, 0x%X };%s" % (
                    f.name, f.off, f.width, f.count, f.stride, f.inner_count, f.inner_stride,
                    "  // float" if f.is_float else ""))
            for k, d in enumerate(dyns):
                w("        // dynamic array %s: %d-byte elements" % (d["name"], d["elem_size"]))
                w("        inline constexpr uint8 Dyn%s = %d;" % (d["name"], k))
                for m, src in zip(d["members"], d["sources"]):
                    w("        inline constexpr UFField %s_%s{ 0x%04X, %d, %d, 0x%X, %d, 0x%X };" % (
                        d["name"], ident(src["name"], hx(src["offset"])), m["off"], m["width"], 1, 0,
                        m["count"], m["stride"]))
            w("    }")
            w("")
            for k, d in enumerate(dyns):
                w("    inline constexpr UFInstr %sDyn%d[] =  // %s, %d-byte elements" % (sname, k, d["name"], d["elem_size"]))
                w("    {")
                for m in d["members"]:
                    w("        %s," % self.emit_instr(m))
                w("    };")
            if dyns:
                w("    inline constexpr UFDynInfo %sDyns[] =" % sname)
                w("    {")
                for k, d in enumerate(dyns):
                    w("        { %d, %sDyn%d, %d },  // %s" % (d["elem_size"], sname, k, len(d["members"]), d["name"]))
                w("    };")
            w("    inline constexpr UFInstr %sCreate[] =" % sname)
            w("    {")
            for d in create:
                w("        %s," % self.emit_instr(d))
            w("    };")
            w("    inline constexpr UFInstr %sUpdate[] =" % sname)
            w("    {")
            for d in update:
                c = "  // %s" % d["what"] if d["op"] == "Unsupported" else ""
                w("        %s,%s" % (self.emit_instr(d), c))
            w("    };")
            plist = ", ".join("%d" % parents.get(b, -1) for b in range(nbits))
            w("    inline constexpr int16 %sParents[%d] = { %s };" % (sname, nbits, plist))
            w("    inline constexpr UFStructInfo %sInfo =" % sname)
            w("    {")
            w("        \"%s\", %s, 0x%04X, %d, %d," % (sname, s["type_mask_bit"], size, nbits, blocks))
            w("        %sCreate, std::size(%sCreate), %sUpdate, std::size(%sUpdate)," % (sname, sname, sname, sname))
            w("        %s, %d, %sParents" % ("%sDyns" % sname if dyns else "nullptr", len(dyns), sname))
            w("    };")
            w("")
            if unsupported:
                self.notes.append("%s: update bits not produced: %s" % (sname, ", ".join(map(str, unsupported))))
        w(FOOTER)
        return "\n".join(self.out) + "\n"


HEADER = """/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// Generated by apps/updatefields/updatefields.py from the FrostLite client's update-field
// descriptors (docs/updatefields.json, read off the 3.4.3.54261 client's own readers).
// Do not edit by hand.

#ifndef UPDATE_FIELDS_DATA_H
#define UPDATE_FIELDS_DATA_H

#include "UpdateFieldsTypes.h"
#include <iterator>

namespace UF
{"""

FOOTER = """}

#endif // UPDATE_FIELDS_DATA_H"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("json", type=Path)
    ap.add_argument("--out", type=Path, default=OUTPUT)
    args = ap.parse_args()
    doc = json.loads(args.json.read_text())
    g = Gen(doc)
    text = g.run()
    args.out.write_text(text, newline="\n")
    for n in g.notes:
        print(n)
    print("wrote", args.out)


if __name__ == "__main__":
    main()
