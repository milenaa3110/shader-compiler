#!/usr/bin/env python3
# spv_count.py — static structural metrics for a SPIR-V binary, with no
# dependency on spirv-tools (which isn't part of this build). Parses the binary
# word stream directly: the 5-word header, then instructions whose first word
# packs (wordCount << 16) | opcode.
#
# Prints one space-separated line:  <bytes> <words> <instructions> <lx> <ly> <lz>
# where lx/ly/lz is the LocalSize execution mode (compute workgroup), or 0 0 0
# if the module declares none. This is the SPIR-V side of compare_backends.sh.

import struct
import sys

SPV_MAGIC = 0x07230203
OP_EXECUTION_MODE = 16   # OpExecutionMode <entry> <mode> [literals...]
MODE_LOCAL_SIZE = 17     # ExecutionMode LocalSize x y z


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: spv_count.py <file.spv>")
    data = open(sys.argv[1], "rb").read()
    nbytes = len(data)
    if nbytes < 20 or nbytes % 4 != 0:
        sys.exit(f"{sys.argv[1]}: not a valid SPIR-V module ({nbytes} bytes)")

    # Word endianness is whatever the producer used; the magic reveals it.
    if struct.unpack_from("<I", data, 0)[0] == SPV_MAGIC:
        endian = "<"
    elif struct.unpack_from(">I", data, 0)[0] == SPV_MAGIC:
        endian = ">"
    else:
        sys.exit(f"{sys.argv[1]}: bad SPIR-V magic")

    nwords = nbytes // 4
    words = struct.unpack(f"{endian}{nwords}I", data)

    i = 5                       # skip the header
    ninstr = 0
    lx = ly = lz = 0
    while i < nwords:
        wc = words[i] >> 16
        op = words[i] & 0xFFFF
        if wc == 0:             # malformed; stop rather than spin
            break
        ninstr += 1
        if op == OP_EXECUTION_MODE and wc >= 6 and words[i + 2] == MODE_LOCAL_SIZE:
            lx, ly, lz = words[i + 3], words[i + 4], words[i + 5]
        i += wc

    print(nbytes, nwords, ninstr, lx, ly, lz)


main()
