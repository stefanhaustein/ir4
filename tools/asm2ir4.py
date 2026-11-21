#!/usr/bin/env python3
"""Convert annotated 6502/Z80 assembly into IR4th 'code ... end' libraries."""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile


def parse_slices(asm_text):
    """Split assembly into shared header lines and [(sig, body_lines), ...]."""
    header = []
    slices = []
    cur_sig = None
    cur_body = []

    for line in asm_text.splitlines():
        s = line.strip()
        if s.startswith("; @code "):
            if cur_sig is not None:
                slices.append((cur_sig, cur_body))
            cur_sig = s[len("; @code "):].strip()
            cur_body = []
        elif cur_sig is None:
            header.append(line)
        else:
            cur_body.append(line)

    if cur_sig is not None:
        slices.append((cur_sig, cur_body))
    if not slices:
        raise RuntimeError("No '; @code <name> ( ... )' markers found")
    return header, slices


def assemble_6502(src_text, org, stk, tmpdir):
    if not shutil.which("ca65") or not shutil.which("ld65"):
        sys.exit("ca65 and ld65 (cc65) are required")

    src_path = os.path.join(tmpdir, f"s_{org:04x}.s")
    obj_path = os.path.join(tmpdir, f"s_{org:04x}.o")
    cfg_path = os.path.join(tmpdir, f"s_{org:04x}.cfg")
    bin_path = os.path.join(tmpdir, f"s_{org:04x}.bin")

    with open(src_path, "w") as f:
        f.write(".code\n" + src_text + "\n")
    with open(cfg_path, "w") as f:
        f.write(
            f"MEMORY {{\n    RAM: start = ${org:04X}, size = $2000, fill = no, file = %O;\n}}\n"
            "SEGMENTS {\n    CODE: load = RAM, type = ro;\n}\n"
        )
    subprocess.run(["ca65", "-D", f"STACK=${stk:04X}", src_path, "-o", obj_path], check=True)
    subprocess.run(["ld65", "-C", cfg_path, obj_path, "-o", bin_path], check=True)
    with open(bin_path, "rb") as f:
        return bytearray(f.read())


def assemble_z80(src_text, org, _stk, tmpdir):
    if not shutil.which("z80asm"):
        sys.exit("z80asm is required")

    src_path = os.path.join(tmpdir, f"s_{org:04x}.asm")
    bin_path = os.path.join(tmpdir, f"s_{org:04x}.bin")
    with open(src_path, "w") as f:
        f.write(f"    ORG 0x{org:04X}\n" + src_text + "\n")
    subprocess.run(["z80asm", f"-o{bin_path}", src_path], check=True)
    with open(bin_path, "rb") as f:
        return bytearray(f.read())


def encode_slice(b0, b1):
    if len(b0) != len(b1):
        raise RuntimeError(f"Binary size mismatch: {len(b0)} vs {len(b1)}")
    toks = []
    i = 0
    while i < len(b0):
        if i + 1 < len(b0) and b0[i + 1] != b1[i + 1] and b0[i] == b1[i]:
            delta = (b1[i + 1] - b0[i + 1]) & 0xFF
            val = b0[i] | (b0[i + 1] << 8)
            if delta == 4:
                toks.append(f"{val - 0x4000} :ar")
            elif delta == 8:
                toks.append(f"{val - 0x8000} :as")
            else:
                raise RuntimeError(f"Unexpected reloc delta {delta} at offset 0x{i:04X}")
            i += 2
        elif b0[i] != b1[i]:
            raise RuntimeError(f"Unexpected diff at offset 0x{i:04X}: 0x{b0[i]:02X} vs 0x{b1[i]:02X}")
        else:
            toks.append(f"0x{b0[i]:02X} :i8")
            i += 1
    return toks


def main():
    parser = argparse.ArgumentParser(description="Generate IR4th code library from .asm.")
    parser.add_argument("--cpu", required=True, choices=["6502", "z80"])
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    with open(args.input, "r") as f:
        header, slices = parse_slices(f.read())

    asm_fn = assemble_6502 if args.cpu == "6502" else assemble_z80
    header_text = "\n".join(header)

    out_lines = [
        f"\\ Auto-generated from {args.input} by tools/asm2ir4.py (do not edit manually).",
        "",
    ]

    total_bytes = 0
    for sig, body in slices:
        src = header_text + "\n" + "\n".join(body)
        with tempfile.TemporaryDirectory() as tmpdir:
            b0 = asm_fn(src, 0x4000, 0x8000, tmpdir)
            b1 = asm_fn(src, 0x4400, 0x8800, tmpdir)
        toks = encode_slice(b0, b1)
        ret_tok = "0x60 :i8" if args.cpu == "6502" else "0xC9 :i8"
        if toks and toks[-1] == ret_tok:
            toks.pop()
        total_bytes += len(b0)
        out_lines.append(f"code {sig}")
        for off in range(0, len(toks), 8):
            out_lines.append("  " + " ".join(toks[off : off + 8]))
        out_lines.append("end")
        out_lines.append("")

    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    with open(args.output, "w") as f:
        f.write("\n".join(out_lines))
    print(f"Generated {args.output}: {len(slices)} code words, {total_bytes} bytes")


if __name__ == "__main__":
    main()
