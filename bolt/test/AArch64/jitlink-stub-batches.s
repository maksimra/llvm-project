## The callers span 160 MiB, but share a forward island after second.
## Reserving immediately after the first caller needs two stubs instead.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %s -o %t.o
# RUN: ld.lld --emit-relocs -e _start %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --turn-off-stubs --lite=0 \
# RUN:   --skip-funcs=callee --align-functions=4 --use-gnu-stack \
# RUN:   --custom-allocation-vma=0x40000000 \
# RUN:   --pad-funcs-before=second:83886080,third:83886080 \
# RUN:   --print-stub-stats --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=STATS
# STATS: BOLT-INFO: JITLink stubs: slots=1 unused=0 short-adrp=1 long-mov=0 shared=2
## The emitted 167772172 bytes plus one 12-byte slot need no extra padding.
# RUN: llvm-readelf -SW %t.bolt | FileCheck %s --check-prefix=SIZE
# SIZE: .text PROGBITS {{[0-9a-f]+}} {{[0-9a-f]+}} a000018
# RUN: llvm-objdump -d --no-show-raw-insn \
# RUN:   --disassemble-symbols=_start,second,third %t.bolt | FileCheck %s

# CHECK-LABEL: <_start>:
# CHECK-NEXT: b 0x[[#%x,STUB:]]
# CHECK-LABEL: <second>:
# CHECK-NEXT: b 0x[[#STUB]]
# CHECK-NEXT: {{0*}}[[#STUB]]: adrp x16,
# CHECK-NEXT: add x16, x16,
# CHECK-NEXT: br x16
# CHECK-LABEL: <third>:
# CHECK-NEXT: b 0x[[#STUB]]

## Targets beyond ADRP range are rejected even in a fixed-address binary.
# RUN: not llvm-bolt %t.exe -o %t.out-of-range --turn-off-stubs --lite=0 \
# RUN:   --skip-funcs=callee --align-functions=4 --use-gnu-stack \
# RUN:   --custom-allocation-vma=0x1234500000 \
# RUN:   --pad-funcs-before=second:83886080,third:83886080 \
# RUN:   --print-stub-stats --verify-branch26-range \
# RUN:   2>&1 | FileCheck %s --check-prefix=OUT-OF-RANGE
# OUT-OF-RANGE: BOLT-ERROR: JITLink failed: Cannot materialize AArch64 branch stub at 0x{{[0-9a-f]+}} for target 0x{{[0-9a-f]+}}: target is outside ADRP range

.text
.globl _start
.type _start, %function
_start:
  b callee
.size _start, .-_start

.globl second
.type second, %function
second:
  b callee
.size second, .-second

.globl third
.type third, %function
third:
  b callee
.size third, .-third

.globl callee
.type callee, %function
callee:
  ret
.size callee, .-callee
