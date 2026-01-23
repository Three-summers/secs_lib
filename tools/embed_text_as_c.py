#!/usr/bin/env python3

import argparse
import pathlib


def c_escape_bytes(data: bytes) -> str:
    # Generate a C string literal containing escaped bytes.
    # Use \xNN for non-printables and special chars.
    out = []
    for b in data:
        if b == 0x0A:  # \n
            out.append("\\n")
        elif b == 0x0D:  # \r
            out.append("\\r")
        elif b == 0x09:  # \t
            out.append("\\t")
        elif b == 0x5C:  # \\
            out.append("\\\\")
        elif b == 0x22:  # "
            out.append('\\"')
        elif 0x20 <= b <= 0x7E:
            out.append(chr(b))
        else:
            out.append(f"\\x{b:02x}")
    return "".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Embed a text/binary file into a generated C header as a string literal.")
    ap.add_argument("--input", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--var", required=True, help="C identifier for the generated variables")
    ap.add_argument(
        "--null-terminate",
        action="store_true",
        help="Append a NUL terminator to the embedded bytes (recommended for text).",
    )
    args = ap.parse_args()

    in_path = pathlib.Path(args.input)
    out_path = pathlib.Path(args.output)
    var = args.var

    data = in_path.read_bytes()
    if args.null_terminate and (len(data) == 0 or data[-1] != 0):
        data = data + b"\x00"

    esc = c_escape_bytes(data)

    # Split into multiple adjacent string literals to avoid extremely long lines.
    chunk = 160
    parts = [esc[i : i + chunk] for i in range(0, len(esc), chunk)]
    literal = "\n".join([f'    "{p}"' for p in parts])

    rel = in_path.as_posix()
    guard = f"SECS_EMBED_{var.upper()}_H_"

    content = (
        f"#pragma once\n"
        f"\n"
        f"// Generated from: {rel}\n"
        f"// DO NOT EDIT MANUALLY.\n"
        f"\n"
        f"#include <stddef.h>\n"
        f"\n"
        f"static const unsigned char {var}_bytes[] =\n{literal};\n"
        f"static const size_t {var}_size = sizeof({var}_bytes);\n"
        f"static const char* {var}_cstr = (const char*){var}_bytes;\n"
    )

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(content, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
