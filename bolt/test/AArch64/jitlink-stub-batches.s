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
# RUN:   | FileCheck %s --check-prefixes=BATCHES,STATS \
# RUN:     --implicit-check-not="BOLT-INFO: JITLink batch:"
## Report the three batches together, including batches reusing an older island.
# BATCHES: BOLT-INFO: JITLink batches: section=.text batches=3 fragments=3 islands=1 slots=1
# BATCHES-NEXT: BOLT-INFO: JITLink layout: section=.text emitted-bytes=167772172 guard-bytes=0 stub-bytes=20 alignment-bytes=0 total-bytes=167772192
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
