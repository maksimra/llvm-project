"""Check the LSDA against final instruction addresses, without extra modules."""

import re
import struct
import sys
from pathlib import Path

elf = Path(sys.argv[1]).read_bytes()
dump = Path(sys.argv[2]).read_text()
start = int(re.search(r"([0-9a-f]+) <metadata>:", dump)[1], 16)
call = int(re.search(r"([0-9a-f]+):\s+bl\s", dump)[1], 16)
landing_pad = int(re.search(r"([0-9a-f]+):\s+mov\s+w0, #0x2a", dump)[1], 16)
fde = re.search(
    rf"FDE cie=.* pc=0*{start:x}\.\.\.([0-9a-f]+)\n"
    r"\s+Format:.*\n\s+LSDA Address: ([0-9a-f]+)",
    dump,
)
end, lsda = (int(value, 16) for value in fde.groups())

# Locate the LSDA's bytes in the ELF64 little-endian section table.
assert elf[:6] == b"\x7fELF\x02\x01"
shoff = struct.unpack_from("<Q", elf, 40)[0]
shentsize, shnum = struct.unpack_from("<HH", elf, 58)
for index in range(shnum):
    _, _, _, address, offset, size, *_ = struct.unpack_from(
        "<IIQQQQIIQQ", elf, shoff + index * shentsize
    )
    if address <= lsda < address + size:
        pos = offset + lsda - address
        break
else:
    raise AssertionError("LSDA not found in any section")


def byte():
    global pos
    value = elf[pos]
    pos += 1
    return value


def uleb():
    value = shift = 0
    while True:
        part = byte()
        value |= (part & 127) << shift
        if not part & 128:
            return value
        shift += 7


# For this single-fragment cleanup LSDA, LPStart is the FDE start and both
# the type table and actions are omitted. BOLT writes ULEB128 call-site entries.
assert byte() == 0xFF, "unexpected LPStart encoding"
assert byte() == 0xFF, "unexpected type-table encoding"
assert byte() == 1, "unexpected call-site encoding"
size = uleb()
table_end = pos + size
matched = False
while pos < table_end:
    begin, length, target, action = uleb(), uleb(), uleb(), uleb()
    assert start + begin + length <= end, "EH range exceeds the FDE"
    if start + begin <= call < start + begin + length:
        assert start + begin == call, "incorrect call-site start"
        assert start + target == landing_pad, "incorrect landing pad"
        assert action == 0, "cleanup action expected"
        matched = True
assert pos == table_end
assert matched, "throwing call is outside every EH range"
