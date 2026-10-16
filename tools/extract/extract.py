#!/usr/bin/env python3
"""Extract PRG and CHR data from the user's own SMB3 (PRG1) ROM.

The ROM is never copied into the repository. This tool checks the dump against
the hash the disassembly was verified with (byte-identical reassembly), then
writes the raw PRG and CHR sections into an asset directory that is ignored by git.

Usage: extract.py --rom "Super Mario Bros. 3 (U) (PRG1) [!].nes" --out assets
"""
import argparse
import hashlib
import json
import os
import sys

EXPECTED_SHA1 = "6bd518e85eb46a4252af07910f61036e84b020d1"
EXPECTED_PRG_SHA1 = "46a9acfc0b2f7c891a90a104d0ea803f96330ced"
EXPECTED_CHR_SHA1 = "2697d1f21b72a6d8e7d2a2d2c51c9c5550f68b56"
PRG_SIZE = 16 * 16 * 1024   # 256 KB
CHR_SIZE = 16 * 8 * 1024    # 128 KB


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    with open(args.rom, "rb") as fh:
        rom = fh.read()
    sha1 = hashlib.sha1(rom).hexdigest()
    if rom[:4] != b"NES\x1a" or rom[4] != 16 or rom[5] != 16 or (rom[6] >> 4) != 4:
        sys.exit("unexpected iNES header")
    if len(rom) != 16 + PRG_SIZE + CHR_SIZE:
        sys.exit("unexpected ROM size %d" % len(rom))

    prg = rom[16:16 + PRG_SIZE]
    chr_data = rom[16 + PRG_SIZE:]

    # The game data is what matters: header variants of the same dump
    # (e.g. No-Intro "(USA) (Rev 1)" vs GoodNES "(U) (PRG1) [!]") carry
    # identical PRG/CHR, so the payload hash is the real check.
    if hashlib.sha1(prg).hexdigest() != EXPECTED_PRG_SHA1 or \
            hashlib.sha1(chr_data).hexdigest() != EXPECTED_CHR_SHA1:
        sys.exit("ROM payload is not the verified SMB3 (U) PRG1 dump "
                 "(prg %.8s.., chr %.8s..)"
                 % (hashlib.sha1(prg).hexdigest(), hashlib.sha1(chr_data).hexdigest()))
    if sha1 != EXPECTED_SHA1:
        print("note: non-canonical header (sha1 %s); payload verified" % sha1)

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "prg.bin"), "wb") as fh:
        fh.write(prg)
    with open(os.path.join(args.out, "chr.bin"), "wb") as fh:
        fh.write(chr_data)
    with open(os.path.join(args.out, "manifest.json"), "w") as fh:
        json.dump({"source_sha1": sha1, "prg_bytes": len(prg), "chr_bytes": len(chr_data),
                   "mapper": 4}, fh, indent=2)
    print("extracted PRG (%d bytes) and CHR (%d bytes) to %s" % (len(prg), len(chr_data), args.out))


if __name__ == "__main__":
    main()
