#!/usr/bin/env python3
"""Encode CacheFlex SPM pseudo-instructions in AArch64 assembly.

This is a cleaned port of ../cacheflex_micro/spm_tools/spm_compiler.py,
trimmed to the instruction forms used by the benchmark kernels:

  SPMCP_64_IMM x_spm, [x_src, #0]
  SPMWB_64_IMM x_spm, [x_dst, #0]
  SPMLDR_8_IMM x_dst, [x_spm, #0]
  SPMSTR_8_IMM x_src, [x_spm, #0]
  SPMREL_8_IMM xzr, [x_spm, #0]
  spm.ld1d z0.d, p0/z, [x_spm]
  spm.st1d z0.d, p0,   [x_spm]

The scalar encoding matches gem5/src/arch/arm/isa/formats/aarch64.isa in this
repo: top byte 0xff, bit[24]=1, op in bits[23:22], size2 in bits[21:20],
addr mode in bits[19:18], ext2 in bits[17:16], imm6 in bits[15:10].
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path


SCALAR_RE = re.compile(
    r"^\s*SPM(CP|WB|LDR|STR|REL)_(\d+)_(IMM|POST|PRE)\s+"
    r"([wx](?:[0-9]|[12][0-9]|3[01])|sp|xzr|wzr),\s*"
    r"\[([wx](?:[0-9]|[12][0-9]|3[01])|sp|xzr|wzr)"
    r"(?:,\s*#?(-?\d+))?\]\s*$",
    re.IGNORECASE,
)

VECTOR_RE = re.compile(
    r"^\s*spm\.(ld1|st1|ld1rq|ld1ro)([bhswdq]|aw|ad|qd|sbw|sbh|sd|sh|sw|sb)\s+"
    r"\{?z([0-9]|[12][0-9]|3[01])\.[bhsd]\}?,\s*"
    r"p([0-7])(?:/z)?,\s*"
    r"\[([wx](?:[0-9]|[12][0-9]|3[01])|sp|xzr|wzr)"
    r"(?:,\s*#?(-?\d+))?(?:,\s*mul\s+vl)?\]\s*$",
    re.IGNORECASE,
)


SCALAR_OP = {"LDR": 0, "STR": 1, "REL": 1, "CP": 2, "WB": 3}
ADDR_MODE = {"IMM": 0, "POST": 1, "PRE": 2}
SIZE2 = {"1": 0, "2": 1, "4": 2, "8": 3, "16": 3, "32": 3, "64": 3}
EXT2 = {"8": 0, "16": 1, "32": 2, "64": 3}

VECTOR_DTYPE = {
    "b": 0x0,
    "h": 0x5,
    "w": 0xA,
    "d": 0xF,
    "q": 0xF,
    "sd": 0x4,
    "sh": 0x8,
    "sw": 0x9,
    "sb": 0xC,
    "sbw": 0xD,
    "sbh": 0xE,
    "aw": 0xA,
    "ad": 0xB,
    "qd": 0xF,
}


def reg_num(reg: str) -> int:
    reg = reg.lower()
    if reg in ("sp", "xzr", "wzr"):
        return 31
    m = re.match(r"[wx](\d+)$", reg)
    if not m:
        raise ValueError(f"bad register {reg!r}")
    value = int(m.group(1))
    if not 0 <= value <= 31:
        raise ValueError(f"register out of range: {reg!r}")
    return value


def encode_scalar(text: str) -> int | None:
    m = SCALAR_RE.match(text)
    if not m:
        return None

    op_name, size_name, mode_name, rt_name, rn_name, imm_text = m.groups()
    op = SCALAR_OP[op_name.upper()]
    if size_name not in SIZE2:
        raise ValueError(f"unsupported SPM size {size_name}")

    size2 = SIZE2[size_name]
    mode = ADDR_MODE[mode_name.upper()]
    ext2 = EXT2.get(size_name, 0)
    # Scalar SPM store and release share op=1 in the custom encoding.  Use the
    # otherwise-unused ext2=3 pattern for release so gem5 can distinguish
    # SPMREL from SPMSTR while keeping scalar stores on ext2=0.
    if op_name.upper() == "REL":
        ext2 = 3
    imm = int(imm_text or 0)

    if op <= 1:
        elem_bytes = 1 << size2
    else:
        elem_bytes = 8 << ext2
    if imm < 0 or imm % elem_bytes != 0:
        raise ValueError(
            f"SPM immediate {imm} must be a non-negative multiple of {elem_bytes}"
        )
    imm6 = imm // elem_bytes
    if not 0 <= imm6 <= 63:
        raise ValueError(f"SPM immediate out of range: {imm}")

    code = 0
    code |= 0xFF << 24
    code |= op << 22
    code |= size2 << 20
    code |= mode << 18
    code |= ext2 << 16
    code |= imm6 << 10
    code |= reg_num(rn_name) << 5
    code |= reg_num(rt_name)

    # gem5 m5ops are decoded by their low 16 bits. Avoid producing an SPM
    # instruction that the decoder intentionally treats as a gem5 pseudo-op.
    if (code & 0xFFFF) == 0x0110:
        raise ValueError(
            f"encoded SPM instruction 0x{code:08x} conflicts with gem5 m5ops"
        )
    return code


def encode_vector(text: str) -> int | None:
    m = VECTOR_RE.match(text)
    if not m:
        return None

    family, suffix, zt_text, pg_text, rn_name, imm_text = m.groups()
    family = family.lower()
    suffix = suffix.lower()
    if suffix not in VECTOR_DTYPE:
        raise ValueError(f"unsupported SPM vector suffix {suffix!r}")

    dtype = VECTOR_DTYPE[suffix]
    imm = int(imm_text or 0)
    elem_size = 1 << (dtype & 0x3)
    if imm % elem_size != 0:
        raise ValueError(
            f"SPM vector immediate {imm} must be a multiple of {elem_size}"
        )
    imm4 = imm // elem_size
    if not -8 <= imm4 <= 7:
        raise ValueError(f"SPM vector immediate out of range: {imm}")

    code = 0
    code |= 1 << 31
    code |= 1 << 26
    code |= (0b01 if family.startswith("ld") else 0b11) << 29
    code |= dtype << 21
    if family in ("ld1", "st1"):
        code |= 1 << 20
    code |= (imm4 & 0xF) << 16
    code |= 0b100 << 13
    code |= int(pg_text) << 10
    code |= reg_num(rn_name) << 5
    code |= int(zt_text)
    return code


def strip_asm_comment(line: str) -> tuple[str, str]:
    idx = line.find("//")
    if idx >= 0:
        return line[:idx].rstrip(), line[idx:].rstrip()
    return line.rstrip(), ""


def encode_line(line: str, line_no: int) -> str:
    body, comment = strip_asm_comment(line)
    if not body.strip():
        return line

    stripped = body.strip()
    if not (
        stripped.upper().startswith("SPM")
        or stripped.lower().startswith("spm.")
    ):
        return line

    code = encode_scalar(stripped)
    if code is None:
        code = encode_vector(stripped)
    if code is None:
        raise ValueError(f"line {line_no}: unsupported SPM instruction: {stripped}")

    indent = line[: len(line) - len(line.lstrip())]
    suffix = f" {comment}" if comment else ""
    return f"{indent}.inst 0x{code:08X}{suffix}\n"


def process(input_path: Path, output_path: Path) -> None:
    out = []
    for line_no, line in enumerate(input_path.read_text().splitlines(True), 1):
        out.append(encode_line(line, line_no))
    output_path.write_text("".join(out))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    process(args.input, args.output)


if __name__ == "__main__":
    main()
