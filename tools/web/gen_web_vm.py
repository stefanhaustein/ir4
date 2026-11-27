#!/usr/bin/env python3
"""Generate C byte array header src/plat_web_vm.h from tools/web/ir4_vm.js."""
import argparse


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", default="tools/web/ir4_vm.js")
    ap.add_argument("--output", default="src/plat_web_vm.h")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = f.read()

    with open(args.output, "w", encoding="utf-8") as out:
        out.write("/* Auto-generated from %s - do not edit */\n" % args.input)
        out.write("static const unsigned char plat_web_vm_js[] = {\n")
        for i in range(0, len(data), 16):
            chunk = data[i : i + 16]
            out.write("    " + ", ".join("0x%02X" % b for b in chunk))
            if i + 16 < len(data):
                out.write(",")
            out.write("\n")
        out.write("};\n")


if __name__ == "__main__":
    main()
