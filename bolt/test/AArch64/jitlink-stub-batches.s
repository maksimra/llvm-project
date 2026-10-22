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
# RUN:   | FileCheck %s --check-prefixes=BATCHES,STATS
# BATCHES: BOLT-INFO: JITLink batch: section=.text fragments=[0, 1) look-ahead-end=2
# BATCHES-NEXT: BOLT-INFO:   1 fragment(s), 4 emitted bytes, section-offsets=[0x0, 0x4)
# BATCHES-NEXT: BOLT-INFO:   1 new stub slot(s), 20 reserved bytes, island-offset=0x5000008
# BATCHES-NEXT: BOLT-INFO: JITLink batch: section=.text fragments=[1, 2) look-ahead-end=2
# BATCHES-NEXT: BOLT-INFO:   1 fragment(s), 83886084 emitted bytes, section-offsets=[0x4, 0x5000008)
# BATCHES-NEXT: BOLT-INFO:   0 new stub slot(s), 0 reserved bytes
# BATCHES-NEXT: BOLT-INFO: JITLink batch: section=.text fragments=[2, 3) look-ahead-end=3
# BATCHES-NEXT: BOLT-INFO:   1 fragment(s), 83886084 emitted bytes, section-offsets=[0x500001c, 0xa000020)
# BATCHES-NEXT: BOLT-INFO:   0 new stub slot(s), 0 reserved bytes
# STATS: BOLT-INFO: JITLink stubs: slots=1 unused=0 short-adrp=1 long-mov=0 shared=2
# RUN: llvm-objdump -d --no-show-raw-insn \
# RUN:   --disassemble-symbols=_start,second,third %t.bolt | FileCheck %s

# CHECK-LABEL: <_start>:
# CHECK-NEXT: b 0x[[#%x,STUB:]]
# CHECK-LABEL: <second>:
# CHECK-NEXT: b 0x[[#STUB]]
# CHECK-NEXT: {{0*}}[[#STUB]]: adrp x16,
# CHECK-NEXT: add x16, x16,
# CHECK-NEXT: br x16
# CHECK-NEXT: nop
# CHECK-NEXT: nop
# CHECK-LABEL: <third>:
# CHECK-NEXT: b 0x[[#STUB]]

## The same shared island also supports absolute MOV stubs beyond ADRP range.
# RUN: llvm-bolt %t.exe -o %t.long --turn-off-stubs --lite=0 \
# RUN:   --skip-funcs=callee --align-functions=4 --use-gnu-stack \
# RUN:   --custom-allocation-vma=0x1234500000 \
# RUN:   --pad-funcs-before=second:83886080,third:83886080 \
# RUN:   --print-stub-stats --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=LONG-STATS
# LONG-STATS: BOLT-INFO: JITLink stubs: slots=1 unused=0 short-adrp=0 long-mov=1 shared=2
# RUN: llvm-objdump -d --no-show-raw-insn \
# RUN:   --disassemble-symbols=_start,second,third %t.long \
# RUN:   | FileCheck %s --check-prefix=LONG
# LONG-LABEL: <_start>:
# LONG-NEXT: b 0x[[#%x,STUB:]]
# LONG-LABEL: <second>:
# LONG-NEXT: b 0x[[#STUB]]
# LONG-NEXT: {{0*}}[[#STUB]]: mov x16,
# LONG-NEXT: movk x16,
# LONG-NEXT: movk x16,
# LONG-NEXT: movk x16,
# LONG-NEXT: br x16
# LONG-LABEL: <third>:
# LONG-NEXT: b 0x[[#STUB]]

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
