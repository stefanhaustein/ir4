#!/usr/bin/env python3
"""Synchronize in-place '\\\\' assembly comments and raw byte tokens in IR4th 'code' blocks."""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FNV1A_INIT = 2166136261
FNV1A_PRIME = 16777619

CPU_FAMILY = {
    "6502": "6502",
    "c64": "6502",
    "x16": "65c02",
    "sim65": "6502",
    "65816": "65816",
    "x16gs": "65816",
    "sim65816": "65816",
    "z80": "z80",
    "cpm": "z80",
    "zx81": "z80",
    "ez80": "ez80",
    "agon": "ez80",
}

CODE_HDR_RE = re.compile(r"^(\s*(?:(?:default|hot)\s+)*code\s+(\S+)\s+\S+\s*\([^)]*\))(.*)$")
VAR_DATA_RE = re.compile(r"^\s*(?:var|data)\s+(\S+)(?:\s|$)")
END_RE = re.compile(r"^(\s*end)(?:\s+\\\\\s*([0-9a-fA-F]{4})\s*|\s*(\\(?!\\).*)?|\s*)$")
LABEL_RE = re.compile(r"^[A-Za-z_.@][A-Za-z0-9_.$@]*:")


def fnv1a_byte(h: int, b: int) -> int:
    return ((h ^ b) * FNV1A_PRIME) & 0xFFFFFFFF


def fnv1a_trimmed_line(h: int, s: str) -> int:
    trimmed = s.strip(" \t\r").encode("latin1")
    for b in trimmed:
        h = fnv1a_byte(h, b)
    return fnv1a_byte(h, 0x0A)


def fnv1a_tokens(toks: list[str]) -> int:
    h = FNV1A_INIT
    for t in toks:
        for b in t.encode("latin1"):
            h = fnv1a_byte(h, b)
        h = fnv1a_byte(h, 0x0A)
    return h


def fold_hash16(asm_h: int, tok_h: int) -> int:
    rot = ((tok_h << 1) | (tok_h >> 31)) & 0xFFFFFFFF
    mix = (asm_h ^ rot) & 0xFFFFFFFF
    return (mix ^ (mix >> 16)) & 0xFFFF


def split_asm_comment(line: str):
    """If line contains a '\\\\' comment token, return (left_part, asm_part); else None."""
    in_quote = False
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if in_quote:
            if c == "\\" and i + 1 < n:
                i += 2
                continue
            if c == '"':
                in_quote = False
            i += 1
            continue
        if c == '"':
            in_quote = True
            i += 1
            continue
        if c == "\\" and (i == 0 or line[i - 1] in " \t"):
            if i + 1 < n and line[i + 1] == "\\":
                if i + 2 == n or line[i + 2] in " \t\r":
                    asm_part = line[i + 2 :]
                    return line[:i], asm_part
                raise RuntimeError(f"Expected whitespace after '\\\\' in line: {line}")
            if i + 1 == n or line[i + 1] in " \t\r":
                return None
        i += 1
    return None


def extract_code_tokens(text: str) -> list[str]:
    """Tokenize left-hand raw code text (stripping normal '\\' comments)."""
    in_quote = False
    cut = len(text)
    i = 0
    while i < len(text):
        c = text[i]
        if in_quote:
            if c == "\\" and i + 1 < len(text):
                i += 2
                continue
            if c == '"':
                in_quote = False
            i += 1
            continue
        if c == '"':
            in_quote = True
            i += 1
            continue
        if c == "\\" and (i == 0 or text[i - 1] in " \t"):
            if i + 1 == len(text) or text[i + 1] in " \t\r":
                cut = i
                break
        i += 1
    return text[:cut].split()


def assemble_ca65(cpu: str, header_lines: list[str], body_lines: list[str], syms: list[str], pass_idx: int):
    if not shutil.which("ca65") or not shutil.which("ld65"):
        return None
    is_816 = cpu == "65816"
    org = 0x4000 if pass_idx == 0 else 0x4400
    stk = 0x8000 if pass_idx == 0 else 0x8800
    sym_base = (0x03A000 if pass_idx == 0 else 0x04B000) if is_816 else (0xA000 if pass_idx == 0 else 0xB000)

    with tempfile.TemporaryDirectory() as tmpdir:
        src_path = os.path.join(tmpdir, "s.s")
        obj_path = os.path.join(tmpdir, "s.o")
        cfg_path = os.path.join(tmpdir, "s.cfg")
        bin_path = os.path.join(tmpdir, "s.bin")
        lbl_path = os.path.join(tmpdir, "s.lbl")

        out = []
        if is_816:
            out.extend([".p816", ".smart", ".a16", ".i16"])
        out.append(f"STACK = ${stk:04X}")
        for idx, sname in enumerate(syms):
            out.append(f"{sname} = ${sym_base + idx * 16:X}")
        out.extend(header_lines)
        out.append(".code")
        for i, bl in enumerate(body_lines):
            out.append(f".export __ir4_L{i}")
            out.append(f"__ir4_L{i}:")
            out.append(bl)
        out.append(f".export __ir4_L{len(body_lines)}")
        out.append(f"__ir4_L{len(body_lines)}:")

        with open(src_path, "w", encoding="utf-8") as f:
            f.write("\n".join(out) + "\n")
        with open(cfg_path, "w", encoding="utf-8") as f:
            f.write(
                f"MEMORY {{\n    RAM: start = ${org:04X}, size = $4000, fill = no, file = %O;\n}}\n"
                "SEGMENTS {\n    CODE: load = RAM, type = ro;\n}\n"
            )

        cpu_arg = "65816" if is_816 else ("65C02" if cpu == "65c02" else "6502")
        subprocess.run(["ca65", "--cpu", cpu_arg, src_path, "-o", obj_path], check=True)
        subprocess.run(["ld65", "-C", cfg_path, "-Ln", lbl_path, obj_path, "-o", bin_path], check=True)

        data = Path(bin_path).read_bytes()
        lmap = {}
        for ln in Path(lbl_path).read_text(encoding="utf-8").splitlines():
            parts = ln.split()
            if len(parts) == 3 and parts[0] == "al" and parts[2].startswith(".__ir4_L"):
                idx = int(parts[2][len(".__ir4_L") :])
                lmap[idx] = int(parts[1], 16) - org
        offsets = [lmap[i] for i in range(len(body_lines) + 1)]
        return data, offsets


def assemble_z80(header_lines: list[str], body_lines: list[str], syms: list[str], pass_idx: int):
    if not shutil.which("z80asm"):
        return None
    org = 0x4000 if pass_idx == 0 else 0x4400
    stk = 0x8000 if pass_idx == 0 else 0x8800
    sym_base = 0xA000 if pass_idx == 0 else 0xB000

    with tempfile.TemporaryDirectory() as tmpdir:
        src_path = os.path.join(tmpdir, "s.asm")
        bin_path = os.path.join(tmpdir, "s.bin")
        lbl_path = os.path.join(tmpdir, "s.lbl")

        out = [f"    ORG 0x{org:04X}", f"STACK: equ 0x{stk:04X}"]
        for idx, sname in enumerate(syms):
            out.append(f"{sname}: equ 0x{sym_base + idx * 16:04X}")
        out.extend(header_lines)
        for i, bl in enumerate(body_lines):
            out.append(f"__ir4_L{i}:")
            out.append(bl)
        out.append(f"__ir4_L{len(body_lines)}:")

        with open(src_path, "w", encoding="utf-8") as f:
            f.write("\n".join(out) + "\n")
        subprocess.run(["z80asm", f"-L{lbl_path}", f"-o{bin_path}", src_path], check=True)

        data = Path(bin_path).read_bytes()
        lmap = {}
        for ln in Path(lbl_path).read_text(encoding="utf-8").splitlines():
            m = re.match(r"^__ir4_L(\d+):\s+equ\s+\$([0-9a-fA-F]+)", ln.strip())
            if m:
                lmap[int(m.group(1))] = int(m.group(2), 16) - org
        offsets = [lmap[i] for i in range(len(body_lines) + 1)]
        return data, offsets


def find_agondev_bin(name: str):
    p = shutil.which(name)
    if p:
        return p
    cand = os.path.expanduser(f"~/opt/agondev/bin/{name}")
    if os.path.isfile(cand) and os.access(cand, os.X_OK):
        return cand
    return None


def assemble_ez80(header_lines: list[str], body_lines: list[str], syms: list[str], pass_idx: int):
    as_bin = find_agondev_bin("ez80-none-elf-as")
    ld = find_agondev_bin("ez80-none-elf-ld") or find_agondev_bin("ez80-ld") or find_agondev_bin("ld.lld")
    objcopy = (
        find_agondev_bin("ez80-none-elf-objcopy")
        or find_agondev_bin("ez80-objcopy")
        or find_agondev_bin("llvm-objcopy")
    )
    nm = find_agondev_bin("ez80-none-elf-nm") or find_agondev_bin("ez80-nm") or find_agondev_bin("llvm-nm")
    mc = None if as_bin else find_agondev_bin("ez80-clang")
    if not ((as_bin or mc) and ld and objcopy and nm):
        return None

    org = 0x044000 if pass_idx == 0 else 0x044400
    sym_base = 0x05A000 if pass_idx == 0 else 0x06B000

    with tempfile.TemporaryDirectory() as tmpdir:
        src_path = os.path.join(tmpdir, "s.s")
        obj_path = os.path.join(tmpdir, "s.o")
        elf_path = os.path.join(tmpdir, "s.elf")
        lds_path = os.path.join(tmpdir, "s.ld")
        bin_path = os.path.join(tmpdir, "s.bin")

        out = ["\t.assume adl = 1", "\t.section .text,\"ax\",@progbits"]
        for idx, sname in enumerate(syms):
            out.append(f"\t.equ {sname}, 0x{sym_base + idx * 16:06X}")
        out.extend(header_lines)
        for i, bl in enumerate(body_lines):
            out.append(f"\t.global __ir4_L{i}")
            out.append(f"__ir4_L{i}:")
            out.append(bl)
        out.append(f"\t.global __ir4_L{len(body_lines)}")
        out.append(f"__ir4_L{len(body_lines)}:")

        with open(src_path, "w", encoding="utf-8") as f:
            f.write("\n".join(out) + "\n")
        with open(lds_path, "w", encoding="utf-8") as f:
            f.write(
                f"SECTIONS {{ . = 0x{org:06X}; .text : {{ *(.text*) }} /DISCARD/ : {{ *(*) }} }}\n"
            )

        if as_bin:
            subprocess.run([as_bin, "-march=ez80+adl", src_path, "-o", obj_path], check=True)
        else:
            subprocess.run([mc, "-c", src_path, "-o", obj_path], check=True)
        subprocess.run([ld, "-T", lds_path, obj_path, "-o", elf_path], check=True)
        subprocess.run([objcopy, "-O", "binary", elf_path, bin_path], check=True)
        nm_out = subprocess.check_output([nm, elf_path], text=True)

        data = Path(bin_path).read_bytes()
        lmap = {}
        for ln in nm_out.splitlines():
            parts = ln.split()
            if len(parts) >= 3 and parts[2].startswith("__ir4_L"):
                idx = int(parts[2][len("__ir4_L") :])
                lmap[idx] = int(parts[0], 16) - org
        offsets = [lmap[i] for i in range(len(body_lines) + 1)]
        return data, offsets


def assemble_block(fam: str, header_lines: list[str], body_lines: list[str], syms: list[str]):
    if fam in ("6502", "65c02", "65816"):
        r0 = assemble_ca65(fam, header_lines, body_lines, syms, 0)
        if r0 is None:
            return None
        r1 = assemble_ca65(fam, header_lines, body_lines, syms, 1)
    elif fam == "z80":
        r0 = assemble_z80(header_lines, body_lines, syms, 0)
        if r0 is None:
            return None
        r1 = assemble_z80(header_lines, body_lines, syms, 1)
    elif fam == "ez80":
        r0 = assemble_ez80(header_lines, body_lines, syms, 0)
        if r0 is None:
            return None
        r1 = assemble_ez80(header_lines, body_lines, syms, 1)
    else:
        raise RuntimeError(f"Unsupported assembly target family: {fam}")

    b0, off0 = r0
    b1, off1 = r1
    if len(b0) != len(b1) or off0 != off1:
        raise RuntimeError("Pass 0 / Pass 1 size or line offset mismatch (branch relaxation across origins?)")

    addr_bytes = 3 if fam in ("65816", "ez80") else 2
    org0 = 0x044000 if fam == "ez80" else 0x4000
    sym0 = 0x03A000 if fam == "65816" else (0x05A000 if fam == "ez80" else 0xA000)

    line_toks = []
    for li in range(len(body_lines)):
        s, e = off0[li], off0[li + 1]
        toks = []
        i = s
        while i < e:
            if addr_bytes == 3 and i + 2 < e and (b0[i : i + 3] != b1[i : i + 3]):
                v0 = b0[i] | (b0[i + 1] << 8) | (b0[i + 2] << 16)
                v1 = b1[i] | (b1[i + 1] << 8) | (b1[i + 2] << 16)
                diff = v1 - v0
                if diff == 0x011000:
                    rem = v0 - sym0
                    idx = rem // 16
                    if rem < 0 or rem % 16 != 0 or idx >= len(syms):
                        raise RuntimeError(f"Symbol relocation with non-zero addend at 0x{i:X}")
                    toks.extend(["&", syms[idx]])
                    i += 3
                    continue
            if i + 1 < e and b0[i + 1] != b1[i + 1] and b0[i] == b1[i]:
                v0 = b0[i] | (b0[i + 1] << 8)
                v1 = b1[i] | (b1[i + 1] << 8)
                diff = (v1 - v0) & 0xFFFF
                if diff == 0x0400 and addr_bytes == 2:
                    toks.extend([str(v0 - (org0 & 0xFFFF)), ":ar"])
                    i += 2
                    continue
                if diff == 0x0800 and fam in ("6502", "65c02"):
                    toks.extend([str(v0 - 0x8000), ":as"])
                    i += 2
                    continue
                if diff == 0x1000 and addr_bytes == 2:
                    rem = v0 - sym0
                    idx = rem // 16
                    if rem < 0 or rem % 16 != 0 or idx >= len(syms):
                        raise RuntimeError(f"Symbol relocation with non-zero addend at 0x{i:X}")
                    toks.extend(["&", syms[idx]])
                    i += 2
                    continue
            if b0[i] != b1[i]:
                raise RuntimeError(f"Unexpected relocation delta at byte 0x{i:X}: 0x{b0[i]:02X} vs 0x{b1[i]:02X}")
            toks.extend([f"0x{b0[i]:02X}", ":i8"])
            i += 1
        line_toks.append(toks)
    return line_toks


def process_file(path: Path, check_only: bool, force: bool) -> bool:
    orig_text = path.read_text(encoding="utf-8")
    lines = orig_text.splitlines()

    syms = []
    seen_syms = set()
    for ln in lines:
        m = VAR_DATA_RE.match(ln)
        if m:
            nm = m.group(1)
            if nm not in seen_syms:
                seen_syms.add(nm)
                syms.append(nm)

    hdr_asm_hash = FNV1A_INIT
    header_asm = []
    out_lines = []
    changed = False
    i = 0
    n = len(lines)

    while i < n:
        ln = lines[i]
        m_code = CODE_HDR_RE.match(ln)
        if not m_code:
            sp = split_asm_comment(ln)
            if sp is not None:
                _, asm_part = sp
                hdr_asm_hash = fnv1a_trimmed_line(hdr_asm_hash, asm_part)
                header_asm.append(asm_part.strip(" \t\r"))
            out_lines.append(ln)
            i += 1
            continue

        target = m_code.group(2)
        block_start = i
        i += 1
        block_lines = []
        end_line = None
        end_indent = ""
        end_hash_str = None

        while i < n:
            m_end = END_RE.match(lines[i])
            if m_end and extract_code_tokens(lines[i])[:1] == ["end"]:
                end_line = lines[i]
                end_indent = m_end.group(1)
                end_hash_str = m_end.group(2)
                i += 1
                break
            block_lines.append(lines[i])
            i += 1

        if end_line is None:
            raise RuntimeError(f"{path}:{block_start + 1}: unterminated 'code' block")

        asm_entries = []  # (idx_in_block, asm_part)
        cur_toks = []
        asm_h = hdr_asm_hash
        for bi, bl in enumerate(block_lines):
            sp = split_asm_comment(bl)
            if sp is not None:
                left_part, asm_part = sp
                asm_h = fnv1a_trimmed_line(asm_h, asm_part)
                asm_entries.append((bi, asm_part))
                cur_toks.extend(extract_code_tokens(left_part))
            else:
                cur_toks.extend(extract_code_tokens(bl))

        if not asm_entries:
            out_lines.append(ln)
            out_lines.extend(block_lines)
            out_lines.append(end_line)
            continue

        cur_tok_h = fnv1a_tokens(cur_toks)
        cur_expected = fold_hash16(asm_h, cur_tok_h)
        cur_tag_ok = end_hash_str is not None and int(end_hash_str, 16) == cur_expected

        fam = CPU_FAMILY.get(target)
        if fam is None:
            if cur_tag_ok and not force:
                out_lines.append(ln)
                out_lines.extend(block_lines)
                out_lines.append(end_line)
                continue
            raise RuntimeError(f"{path}:{block_start + 1}: unsupported target '{target}' for '\\\\' assembly")

        if cur_tag_ok and not force:
            out_lines.append(ln)
            out_lines.extend(block_lines)
            out_lines.append(end_line)
            continue

        asm_src_lines = [ae[1].strip(" \t\r") for ae in asm_entries]
        per_line_toks = assemble_block(fam, header_asm, asm_src_lines, syms)
        if per_line_toks is None:
            if cur_tag_ok:
                out_lines.append(ln)
                out_lines.extend(block_lines)
                out_lines.append(end_line)
                continue
            raise RuntimeError(
                f"{path}:{block_start + 1}: '{target}' '\\\\' assembly modified, "
                f"but required '{fam}' assembler is not installed"
            )

        all_new_toks = [t for lt in per_line_toks for t in lt]
        new_tok_h = fnv1a_tokens(all_new_toks)
        new_hash16 = fold_hash16(asm_h, new_tok_h)

        tok_strs = ["  " + " ".join(lt) if lt else "" for lt in per_line_toks]
        col = max([32] + [len(ts) + 2 for ts in tok_strs if ts])

        asm_map = {asm_entries[k][0]: (tok_strs[k], asm_entries[k][1]) for k in range(len(asm_entries))}
        new_block_lines = []
        for bi, bl in enumerate(block_lines):
            if bi in asm_map:
                ts, raw_asm = asm_map[bi]
                asm_clean = raw_asm.strip(" \t\r\n")
                if asm_clean:
                    pad = " " if LABEL_RE.match(asm_clean) else "   "
                    asm_clean = pad + asm_clean
                new_block_lines.append(f"{ts:<{col}}\\\\{asm_clean}".rstrip())
            else:
                if not extract_code_tokens(bl):
                    new_block_lines.append(bl)

        new_end_line = f"{end_indent} \\\\ {new_hash16:04x}"
        out_lines.append(ln)
        out_lines.extend(new_block_lines)
        out_lines.append(new_end_line)
        if new_block_lines != block_lines or new_end_line != end_line:
            changed = True

    new_text = "\n".join(out_lines) + ("\n" if orig_text.endswith("\n") else "")
    if new_text != orig_text:
        changed = True
        if check_only:
            print(f"OUT OF SYNC: {path}", file=sys.stderr)
        else:
            path.write_text(new_text, encoding="utf-8")
            print(f"Synced {path}")
    return changed


def main():
    ap = argparse.ArgumentParser(description="Sync '\\\\' assembly comments in .ir4 code blocks.")
    ap.add_argument("--check", action="store_true", help="Fail if any file is out of sync")
    ap.add_argument("--force", action="store_true", help="Re-assemble even when hash matches")
    ap.add_argument("--since", help="Only inspect files newer than this stamp path")
    ap.add_argument("files", nargs="*", help=".ir4 files to process")
    args = ap.parse_args()

    paths = [Path(p) for p in args.files]
    if not paths:
        root = Path(__file__).resolve().parent.parent / "lib"
        paths = sorted(root.rglob("*.ir4"))

    if args.since and os.path.exists(args.since) and not args.force and not args.check:
        stamp_mtime = os.path.getmtime(args.since)
        script_mtime = os.path.getmtime(__file__)
        if script_mtime <= stamp_mtime:
            paths = [p for p in paths if p.exists() and os.path.getmtime(p) > stamp_mtime]

    any_changed = False
    for p in paths:
        if p.exists():
            if process_file(p, args.check, args.force):
                any_changed = True

    if args.check and any_changed:
        sys.exit(1)


if __name__ == "__main__":
    main()
