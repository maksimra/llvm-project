"""Follow encoded B/BL instructions through anonymous thunks in an AArch64 ELF."""

import struct
import subprocess
import sys

nm, path, expected_hops = sys.argv[1:]
symbols = {}
for line in subprocess.check_output([nm, "--defined-only", path], text=True).splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbols[fields[2]] = int(fields[0], 16)

with open(path, "rb") as elf:
    header = struct.unpack("<16sHHIQQQIHHHHHH", elf.read(64))
    assert header[0][:6] == b"\x7fELF\x02\x01"
    segments = []
    for index in range(header[10]):
        elf.seek(header[5] + index * header[9])
        segment = struct.unpack("<IIQQQQQQ", elf.read(56))
        if segment[0] == 1:
            segments.append(segment)

    def instruction(address):
        for segment in segments:
            offset, base, size = segment[2], segment[3], segment[5]
            if base <= address and address + 4 <= base + size:
                elf.seek(offset + address - base)
                return struct.unpack("<I", elf.read(4))[0]
        raise AssertionError(f"Unmapped instruction at {address:#x}")

    for source, target in [("_start", "callee"), ("callee", "_start")]:
        address, destination = symbols[source], symbols[target]
        for hop in range(int(expected_hops) + 1):
            opcode = instruction(address)
            mask = 0x7C000000 if hop == 0 else 0xFC000000
            assert opcode & mask == 0x14000000, f"Expected B at {address:#x}"
            immediate = opcode & 0x3FFFFFF
            if immediate & 0x2000000:
                immediate -= 0x4000000
            address += immediate * 4
            assert address != destination or hop == int(expected_hops)
        assert address == destination, f"Chain from {source} missed {target}"
