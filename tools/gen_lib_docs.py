#!/usr/bin/env python3
"""Extract top-level library documentation from lib/*.ir4 into docs/lib.md."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
LIB_DIR = ROOT / "lib"
DOCS_DIR = ROOT / "docs" / "lib"
OUT_PATH = DOCS_DIR / "README.md"

ORDER = [
    "stdio.ir4",
    "mem.ir4",
    "heap.ir4",
    "math.ir4",
    "i32.ir4",
    "gfx.ir4",
    "sprite.ir4",
    "sfx.ir4",
    "joy.ir4",
]

PLATFORMS = sorted(["c64", "x16", "agon", "zx81", "cpm"])

FN_RE = re.compile(r"^((?:(?:default|hot)\s+)*)fn\s+(\S+)\s*\(([^)]*)\)")
CONST_RE = re.compile(r"^(?:default\s+)?const\s+(\S+)\s+\S+\s+(:\S+)")


def parse_lib(path: Path):
    lines = path.read_text(encoding="utf-8").splitlines()
    header = []
    i = 0
    while i < len(lines) and lines[i].startswith("\\"):
        raw = lines[i][2:] if lines[i].startswith("\\ ") else lines[i][1:]
        header.append(raw.rstrip())
        i += 1

    entries = []
    pending = []
    for line in lines[i:]:
        stripped = line.strip()
        if stripped.startswith("\\"):
            pending.append(stripped[1:].strip())
            continue
        m = FN_RE.match(stripped)
        if m:
            name, sig = m.group(2), m.group(3).strip()
            if not name.startswith("__"):
                doc = " ".join(pending)
                sig_str = f"**{name}** ( {sig} )" if sig else f"**{name}** ( )"
                entries.append((name, sig_str, doc))
            pending = []
            continue
        m = CONST_RE.match(stripped)
        if m:
            name, ty = m.group(1), m.group(2)
            if not name.startswith("__"):
                doc = " ".join(pending)
                sig_str = f"**{name}** ( -> {ty} )"
                entries.append((name, sig_str, doc))
            pending = []
            continue
        if stripped == "":
            pending = []
        elif not stripped.startswith(("var ", "data ", "require ", "fwd ")):
            pending = []
    return header, entries


def strip_mod_prefix(line: str, name: str, plat: str = "") -> str:
    prefixes = [f"lib/{name} - ", f"{name} - "]
    if plat:
        prefixes = [f"lib/{plat}/{name} - ", f"{plat}/{name} - "] + prefixes
    for pfx in prefixes:
        if line.startswith(pfx):
            return line[len(pfx):]
    return line


def collect_platform_notes(mod_name: str):
    headers: list[tuple[str, list[str]]] = []
    notes: dict[str, list[tuple[str, str]]] = {}
    for plat in PLATFORMS:
        p = LIB_DIR / plat / mod_name
        if not p.exists():
            continue
        hdr, entries = parse_lib(p)
        if hdr:
            full_hdr = [strip_mod_prefix(h, mod_name, plat) for h in hdr]
            while full_hdr and not full_hdr[0]:
                full_hdr.pop(0)
            while full_hdr and not full_hdr[-1]:
                full_hdr.pop()
            if full_hdr:
                headers.append((plat, full_hdr))
        for name, _, doc in entries:
            if doc:
                notes.setdefault(name, []).append((plat, doc))
    return headers, notes


def main():
    files = sorted(LIB_DIR.glob("*.ir4"), key=lambda p: p.stem.lower())

    index_out = [
        "# IR4(th) Companion Library",
        "",
        "> Generated from `lib/*.ir4` by `tools/gen_lib_docs.py`.",
        "",
        "## Naming Convention",
        "",
        "- **Public words & scalar constants:** Lowercase without underscores (`typeiw`, `sprmaxw`, `fsopen`).",
        "- **Enum / option constants:** `<group>_<value>` in lowercase (`fsmode_read`, `fsmode_write`).",
        "- **Internal library symbols:** Prefixed with `__` (`__gfx_w`, `__c64_modes`, `__heap_ptr`).",
        "- **Frontend symbols:** Language frontends targeting IR4(th) are expected to mangle or prefix user-defined names to avoid colliding with built-in keywords, companion library words, or `__` internals.",
        "",
        "## Modules",
        "",
    ]

    parsed = [(p, *parse_lib(p)) for p in files]
    DOCS_DIR.mkdir(parents=True, exist_ok=True)

    for p, header, entries in parsed:
        mod_stem = p.stem
        summary = strip_mod_prefix(header[0], p.name) if header else ""
        index_out.append(f"- [{mod_stem}]({mod_stem}.md) — {summary}")

        mod_out = [
            f"# {mod_stem}",
            "",
            f"[← Library Index](README.md) · Source: [lib/{p.name}](../../lib/{p.name})",
            "",
        ]
        body_hdr = [strip_mod_prefix(h, p.name) for h in header]
        if body_hdr:
            mod_out.extend(body_hdr)
            mod_out.append("")
        plat_headers, plat_notes = collect_platform_notes(p.name)
        mod_out.append("## Functions")
        mod_out.append("")
        for name, sig_md, doc in sorted(entries, key=lambda e: e[0].lower()):
            pnotes = plat_notes.get(name, [])
            if doc and len(sig_md) + len(doc) <= 88:
                mod_out.append(f"- {sig_md} — {doc}")
            else:
                mod_out.append(f"- {sig_md}")
                if doc:
                    mod_out.append("")
                    mod_out.append(f"  {doc}")
            for plat, pdoc in pnotes:
                mod_out.append(f"  - *{plat}:* {pdoc}")
            if not (doc and len(sig_md) + len(doc) <= 88 and not pnotes):
                mod_out.append("")
        if plat_headers:
            if mod_out and mod_out[-1] != "":
                mod_out.append("")
            mod_out.append("## Platforms")
            mod_out.append("")
            for plat, phdr in plat_headers:
                mod_out.append(f"### {plat}")
                mod_out.append("")
                mod_out.extend(phdr)
                mod_out.append("")
        if mod_out and mod_out[-1] != "":
            mod_out.append("")
        (DOCS_DIR / f"{mod_stem}.md").write_text("\n".join(mod_out), encoding="utf-8")

    index_out.append("")
    OUT_PATH.write_text("\n".join(index_out), encoding="utf-8")
    print(f"Wrote {DOCS_DIR.relative_to(ROOT)}/*.md ({len(parsed)} modules)")


if __name__ == "__main__":
    main()
