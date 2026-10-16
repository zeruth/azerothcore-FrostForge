"""The Battle.net .proto files, recovered from the 3.4.3.54261 client itself.

The client's generated protobuf code keeps each .proto file's descriptor as a serialized
FileDescriptorProto in its read-only data (registered with the descriptor pool at startup). Every
message, enum and service the client speaks is described there exactly: field numbers, types,
labels, defaults, packed fields, and the bgs custom options that carry each RPC method's id and
each service's descriptor name (whose FNV-1a hash is the service hash on the wire).

This finds every such descriptor in the exe and writes it back out as .proto source, one file per
descriptor under its original path, so the server's Battle.net service can be generated with
protoc from the client's own definitions.

    python apps/bnetproto/protoextract.py <WowClassic.exe> <out dir> [--filter bgs/]
    python apps/bnetproto/protoextract.py <WowClassic.exe> - --show bgs/low/pb/client/rpc_types.proto

A descriptor is found by its first field, `0x0a <length> <path>.proto`, and parsed forward as
top-level FileDescriptorProto fields until one does not fit (or a second file name starts).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

TYPES = {1: "double", 2: "float", 3: "int64", 4: "uint64", 5: "int32", 6: "fixed64", 7: "fixed32", 8: "bool",
         9: "string", 10: "group", 11: "message", 12: "bytes", 13: "uint32", 14: "enum", 15: "sfixed32",
         16: "sfixed64", 17: "sint32", 18: "sint64"}
LABELS = {1: "optional", 2: "required", 3: "repeated"}
# FileDescriptorProto: the top-level fields and their wire types
FILE_FIELDS = {1: 2, 2: 2, 3: 2, 4: 2, 5: 2, 6: 2, 7: 2, 8: 2, 9: 2, 10: 0, 11: 0, 12: 2}
# the options messages, by the field that holds them in their owner, for custom option lookup
OPTION_EXTENDEE = {"file": ".google.protobuf.FileOptions", "message": ".google.protobuf.MessageOptions",
                   "field": ".google.protobuf.FieldOptions", "enum": ".google.protobuf.EnumOptions",
                   "enum_value": ".google.protobuf.EnumValueOptions",
                   "service": ".google.protobuf.ServiceOptions", "method": ".google.protobuf.MethodOptions"}


def varint(b: bytes, i: int) -> tuple[int, int]:
    v = s = 0
    while True:
        if i >= len(b) or s > 63:
            raise ValueError("varint")
        c = b[i]
        i += 1
        v |= (c & 0x7f) << s
        s += 7
        if c < 0x80:
            return v, i


def fields(b: bytes) -> list[tuple[int, int, object]]:
    """(number, wire type, value) of every field; raises when the bytes are not a message."""
    i, out = 0, []
    while i < len(b):
        key, i = varint(b, i)
        f, wt = key >> 3, key & 7
        if f == 0:
            raise ValueError("field 0")
        if wt == 0:
            v, i = varint(b, i)
        elif wt == 2:
            n, i = varint(b, i)
            if i + n > len(b):
                raise ValueError("length")
            v = b[i:i + n]
            i += n
        elif wt == 5:
            v = int.from_bytes(b[i:i + 4], "little")
            i += 4
        elif wt == 1:
            v = int.from_bytes(b[i:i + 8], "little")
            i += 8
        else:
            raise ValueError(f"wire type {wt}")
        out.append((f, wt, v))
    if i != len(b):
        raise ValueError("overrun")
    return out


def first(fs, n, default=None):
    return next((v for f, _, v in fs if f == n), default)


def every(fs, n):
    return [v for f, _, v in fs if f == n]


def text(v) -> str:
    return v.decode("utf-8") if isinstance(v, bytes) else str(v)


def find_descriptors(image: bytes) -> list[tuple[int, bytes]]:
    """(offset, serialized FileDescriptorProto) for every descriptor in the image."""
    found = []
    for m in re.finditer(rb"\x0a([\x05-\x7f])([\w/.\-]+\.proto)\x12", image):
        if m.group(1)[0] != len(m.group(2)):
            continue
        start = m.start()
        i, seen = start, set()
        while i < len(image):
            try:
                key, j = varint(image, i)
                f, wt = key >> 3, key & 7
                if f not in FILE_FIELDS or FILE_FIELDS[f] != wt or (f == 1 and 1 in seen) or (f == 2 and 2 in seen):
                    break
                if wt == 2:
                    n, j = varint(image, j)
                    if j + n > len(image):
                        break
                    body = image[j:j + n]
                    if f in (4, 5, 6, 7, 8):
                        fields(body)          # must parse as a message
                    elif f in (2, 3):
                        body.decode("utf-8")
                    j += n
                else:
                    _, j = varint(image, j)
            except (ValueError, UnicodeDecodeError):
                break
            seen.add(f)
            i = j
        found.append((start, image[start:i]))
    return found


class Options:
    """Custom options (the bgs extensions) by extendee and number, decoded with their types."""

    def __init__(self) -> None:
        self.extensions: dict[tuple[str, int], tuple[str, list]] = {}  # -> (full name, field descriptor)
        self.messages: dict[str, list] = {}                               # full name -> DescriptorProto
        self.enums: dict[str, list] = {}
        # The file each extension is defined in, and the files of the extensions rendered since
        # `used` was last cleared. The client's descriptors do not list a file whose only use is
        # an option (account_service.proto never names method_options.proto), and protoc rejects
        # the option without the import, so render_file adds them.
        self.extension_file: dict[tuple[str, int], str] = {}
        self.used: set[str] = set()
        self.file = ""

    def learn(self, fd: list) -> None:
        package = text(first(fd, 2, b""))
        prefix = "." + package if package else ""
        self.file = text(first(fd, 1))

        def walk(msg: list, scope: str) -> None:
            name = f"{scope}.{text(first(msg, 1))}"
            self.messages[name] = msg
            for nested in every(msg, 3):
                walk(fields(nested), name)
            for e in every(msg, 4):
                ef = fields(e)
                self.enums[f"{name}.{text(first(ef, 1))}"] = ef
            for x in every(msg, 6):
                self.extension(fields(x), name)

        for m in every(fd, 4):
            walk(fields(m), prefix)
        for e in every(fd, 5):
            ef = fields(e)
            self.enums[f"{prefix}.{text(first(ef, 1))}"] = ef
        for x in every(fd, 7):
            self.extension(fields(x), prefix)

    def extension(self, xf: list, scope: str) -> None:
        key = (text(first(xf, 2)), first(xf, 3))
        self.extensions[key] = (f"{scope}.{text(first(xf, 1))}".lstrip("."), xf)
        self.extension_file[key] = self.file

    def render(self, kind: str, raw: bytes) -> list[str]:
        """`option` assignments for an options message: the standard ones this tool knows, the
        custom ones by their extension, anything else as a comment."""
        out = []
        for f, wt, v in fields(raw):
            if kind == "field" and f == 2:
                out.append(f"packed = {'true' if v else 'false'}")
            elif (kind, f) in (("field", 3), ("message", 3), ("enum", 3), ("enum_value", 1), ("service", 33),
                               ("method", 33)):
                out.append(f"deprecated = {'true' if v else 'false'}")
            elif kind == "enum" and f == 2:
                out.append(f"allow_alias = {'true' if v else 'false'}")
            elif kind == "message" and f == 1:
                out.append(f"message_set_wire_format = {'true' if v else 'false'}")
            elif kind == "file" and f == 1:
                out.append(f'java_package = "{text(v)}"')
            elif kind == "file" and f == 8:
                out.append(f'java_outer_classname = "{text(v)}"')
            elif kind == "file" and f in (17, 18):
                out.append(f"{'java' if f == 17 else 'py'}_generic_services = {'true' if v else 'false'}")
            elif kind == "file" and f == 10:
                out.append(f"java_multiple_files = {'true' if v else 'false'}")
            elif kind == "file" and f == 9:
                out.append(f"optimize_for = {({1: 'SPEED', 2: 'CODE_SIZE', 3: 'LITE_RUNTIME'}).get(v, v)}")
            elif kind == "file" and f == 16:
                out.append(f"cc_generic_services = {'true' if v else 'false'}")
            elif (OPTION_EXTENDEE[kind], f) in self.extensions:
                name, xf = self.extensions[(OPTION_EXTENDEE[kind], f)]
                self.used.add(self.extension_file[(OPTION_EXTENDEE[kind], f)])
                out.extend(self.value(f"({name})", xf, wt, v))
            else:
                out.append(f"/* unknown {kind} option {f} */")
        return out

    def value(self, path: str, fd: list, wt: int, v) -> list[str]:
        t = first(fd, 5)
        if t == 11:                       # a message: one assignment per set field
            msg = self.messages.get(text(first(fd, 6)))
            if msg is None:
                return [f"/* {path}: message {text(first(fd, 6))} not found */"]
            byn = {first(fields(x), 3): fields(x) for x in every(msg, 2)}
            out = []
            for f2, wt2, v2 in fields(v):
                sub = byn.get(f2)
                if sub is None:
                    out.append(f"/* {path} field {f2} */")
                else:
                    out.extend(self.value(f"{path}.{text(first(sub, 1))}", sub, wt2, v2))
            return out
        return [f"{path} = {self.scalar(fd, wt, v)}"]

    def scalar(self, fd: list, wt: int, v) -> str:
        t = first(fd, 5)
        if t in (9,):
            return '"' + text(v).replace("\\", "\\\\").replace('"', '\\"') + '"'
        if t == 12:
            return '"' + "".join(f"\\x{c:02x}" for c in v) + '"'
        if t == 8:
            return "true" if v else "false"
        if t == 14:
            e = self.enums.get(text(first(fd, 6)))
            if e:
                for val in every(e, 2):
                    vf = fields(val)
                    if first(vf, 2, 0) == v:
                        return text(first(vf, 1))
            return str(v)
        if t in (5, 3) and v >= 1 << 63:
            return str(v - (1 << 64))
        if t == 2 and wt == 5:
            import struct
            return repr(struct.unpack("<f", v.to_bytes(4, "little"))[0])
        if t == 1 and wt == 1:
            import struct
            return repr(struct.unpack("<d", v.to_bytes(8, "little"))[0])
        return str(v)


def render_file(fd: list, opts: Options) -> str:
    lines = []
    opts.used = set()
    package = first(fd, 2)
    if package:
        lines.append(f"package {text(package)};")
        lines.append("")
    if first(fd, 8):
        for o in opts.render("file", first(fd, 8)):
            lines.append(f"option {o};" if not o.startswith("/*") else o)
        lines.append("")

    def option_suffix(kind: str, raw) -> str:
        if not raw:
            return ""
        parts = [o for o in opts.render(kind, raw) if not o.startswith("/*")]
        return f" [{', '.join(parts)}]" if parts else ""

    def field_line(ff: list, indent: str, in_oneof: bool = False) -> str:
        t = first(ff, 5)
        type_name = text(first(ff, 6)) if first(ff, 6) else TYPES.get(t, str(t))
        label = "" if in_oneof else (LABELS.get(first(ff, 4), "optional") + " ")
        extras = []
        if first(ff, 7) is not None:
            d = text(first(ff, 7))
            if t == 9:
                d = '"' + d.replace("\\", "\\\\").replace('"', '\\"') + '"'
            elif t == 12:
                d = '"' + d + '"'
            extras.append(f"default = {d}")
        if first(ff, 8):
            extras.extend(o for o in opts.render("field", first(ff, 8)) if not o.startswith("/*"))
        suffix = f" [{', '.join(extras)}]" if extras else ""
        return f"{indent}{label}{type_name} {text(first(ff, 1))} = {first(ff, 3)}{suffix};"

    def enum(ef: list, indent: str) -> None:
        lines.append(f"{indent}enum {text(first(ef, 1))} {{")
        if first(ef, 3):
            for o in opts.render("enum", first(ef, 3)):
                lines.append(f"{indent}  option {o};" if not o.startswith("/*") else f"{indent}  {o}")
        for val in every(ef, 2):
            vf = fields(val)
            n = first(vf, 2, 0)
            if n >= 1 << 63:
                n -= 1 << 64
            lines.append(f"{indent}  {text(first(vf, 1))} = {n}{option_suffix('enum_value', first(vf, 3))};")
        lines.append(f"{indent}}}")

    def extend_blocks(xs: list, indent: str) -> None:
        by_extendee: dict[str, list] = {}
        for x in xs:
            xf = fields(x)
            by_extendee.setdefault(text(first(xf, 2)), []).append(xf)
        for extendee, xfs in by_extendee.items():
            lines.append(f"{indent}extend {extendee} {{")
            for xf in xfs:
                lines.append(field_line(xf, indent + "  "))
            lines.append(f"{indent}}}")

    def message(mf: list, indent: str) -> None:
        lines.append(f"{indent}message {text(first(mf, 1))} {{")
        if first(mf, 7):
            for o in opts.render("message", first(mf, 7)):
                lines.append(f"{indent}  option {o};" if not o.startswith("/*") else f"{indent}  {o}")
        for nested in every(mf, 3):
            message(fields(nested), indent + "  ")
        for e in every(mf, 4):
            enum(fields(e), indent + "  ")
        oneofs = [text(first(fields(o), 1)) for o in every(mf, 8)]
        fs = [fields(x) for x in every(mf, 2)]
        done_oneof = set()
        for ff in fs:
            index = first(ff, 9)
            if index is None:
                lines.append(field_line(ff, indent + "  "))
            elif index not in done_oneof:
                done_oneof.add(index)
                lines.append(f"{indent}  oneof {oneofs[index]} {{")
                for g in fs:
                    if first(g, 9) == index:
                        lines.append(field_line(g, indent + "    ", in_oneof=True))
                lines.append(f"{indent}  }}")
        for r in every(mf, 5):
            rf = fields(r)
            lines.append(f"{indent}  extensions {first(rf, 1)} to {first(rf, 2) - 1};")
        for r in every(mf, 9):
            rf = fields(r)
            lines.append(f"{indent}  reserved {first(rf, 1)} to {first(rf, 2) - 1};")
        if every(mf, 6):
            extend_blocks(every(mf, 6), indent + "  ")
        lines.append(f"{indent}}}")

    for e in every(fd, 5):
        enum(fields(e), "")
        lines.append("")
    for m in every(fd, 4):
        message(fields(m), "")
        lines.append("")
    if every(fd, 7):
        extend_blocks(every(fd, 7), "")
        lines.append("")
    for s in every(fd, 6):
        sf = fields(s)
        lines.append(f"service {text(first(sf, 1))} {{")
        if first(sf, 3):
            for o in opts.render("service", first(sf, 3)):
                lines.append(f"  option {o};" if not o.startswith("/*") else f"  {o}")
        for mth in every(sf, 2):
            mf = fields(mth)
            head = f"  rpc {text(first(mf, 1))}({text(first(mf, 2))}) returns ({text(first(mf, 3))})"
            if first(mf, 4):
                lines.append(head + " {")
                for o in opts.render("method", first(mf, 4)):
                    lines.append(f"    option {o};" if not o.startswith("/*") else f"    {o}")
                lines.append("  }")
            else:
                lines.append(head + ";")
        lines.append("}")
        lines.append("")
    imports = [text(imp) for imp in every(fd, 3)]
    public = {imports[i] for i in every(fd, 10)}  # public_dependency: indexes into the imports
    imports += sorted(opts.used - set(imports) - {text(first(fd, 1))})
    head = [f'syntax = "{text(first(fd, 12, b"proto2"))}";', ""]
    head += [f'import {"public " if imp in public else ""}"{imp}";' for imp in imports]
    if imports:
        head.append("")
    return "\n".join(head + lines).rstrip() + "\n"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("out", help="output directory, or - to print")
    ap.add_argument("--filter", default="", help="only files whose path starts with this")
    ap.add_argument("--show", help="print one file")
    args = ap.parse_args()
    image = Path(args.exe).read_bytes()
    files: dict[str, list] = {}
    for _, raw in find_descriptors(image):
        fd = fields(raw)
        name = text(first(fd, 1))
        # a name can occur twice (a copy in another table); keep the larger
        if name not in files or len(raw) > sum(1 for _ in files[name]):
            files[name] = fd
    opts = Options()
    for fd in files.values():
        opts.learn(fd)
    chosen = sorted(n for n in files if n.startswith(args.filter))
    if args.show:
        sys.stdout.write(render_file(files[args.show], opts))
        return
    if args.out == "-":
        for n in chosen:
            print(n)
        print(f"{len(chosen)} of {len(files)} descriptors")
        return
    out = Path(args.out)
    for n in chosen:
        path = out / n
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(render_file(files[n], opts), encoding="utf-8", newline="\n")
    print(f"{len(chosen)} .proto files written under {out} ({len(files)} descriptors in the exe)")


if __name__ == "__main__":
    main()
