## Deferred linking emits exactly the selected stub form, without NOP tails.
## B-only bridges may move to another fragment boundary before final mapping.

# REQUIRES: system-linux
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %t/short.s -o %t.o
# RUN: ld.lld --emit-relocs -e _start %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.adrp --turn-off-stubs --lite=0 --skip-funcs=callee \
# RUN:   --custom-allocation-vma=0x40000000 --use-gnu-stack \
# RUN:   --print-stub-stats --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=ADRP-STATS
# ADRP-STATS: JITLink compaction: {{.*}}removed-bytes=8 stub-bytes=12 fallback=0
# ADRP-STATS: JITLink stubs: slots=1 unused=0 short-adrp=1 long-mov=0 shared=0 direct-b=0 retained=1 final-bytes=12
# RUN: llvm-objdump -d --section=.text --no-show-raw-insn %t.adrp \
# RUN:   | FileCheck %s --check-prefix=ADRP --implicit-check-not=nop
# ADRP-LABEL: <_start>:
# ADRP-NEXT: b 0x[[#%x,SHORT:]]
# ADRP-NEXT: {{0*}}[[#SHORT]]: adrp x16,
# ADRP-NEXT: add x16, x16,
# ADRP-NEXT: br x16

# RUN: llvm-bolt %t.exe -o %t.mov --turn-off-stubs --lite=0 --skip-funcs=callee \
# RUN:   --custom-allocation-vma=0x1234500000 --use-gnu-stack \
# RUN:   --print-stub-stats --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=MOV-STATS
# MOV-STATS: JITLink compaction: {{.*}}removed-bytes=0 stub-bytes=20 fallback=0
# MOV-STATS: JITLink stubs: slots=1 unused=0 short-adrp=0 long-mov=1 shared=0 direct-b=0 retained=1 final-bytes=20
# RUN: llvm-objdump -d --section=.text --no-show-raw-insn %t.mov \
# RUN:   | FileCheck %s --check-prefix=MOV --implicit-check-not=nop
# MOV-LABEL: <_start>:
# MOV-NEXT: b 0x[[#%x,LONG:]]
# MOV-NEXT: {{0*}}[[#LONG]]: mov x16,
# MOV-NEXT: movk x16,
# MOV-NEXT: movk x16,
# MOV-NEXT: movk x16,
# MOV-NEXT: br x16

## Three whole fragments span 160 MiB. Both calls are out of direct range.
## Forward and backward B bridges fit at the boundary around 80 MiB.
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %t/bridge.s -o %t.b.o
# RUN: ld.lld --emit-relocs -e _start %t.b.o -o %t.b.exe
# RUN: llvm-bolt %t.b.exe -o %t.bridge --turn-off-stubs --lite=0 \
# RUN:   --print-stub-stats --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=BRIDGE-STATS
# BRIDGE-STATS: JITLink compaction: {{.*}}stub-bytes=8 fallback=0
# BRIDGE-STATS: JITLink stubs: {{.*}}short-adrp=0 long-mov=0 {{.*}}direct-b=2 retained=2 final-bytes=8
# RUN: %python %p/Inputs/check-branch-chain.py llvm-nm %t.bridge 1
# RUN: llvm-objdump -d --no-show-raw-insn --disassemble-symbols=_start %t.bridge \
# RUN:   | FileCheck %s --check-prefix=BRIDGE
# BRIDGE-LABEL: <_start>:
# BRIDGE-NEXT: bl 0x[[#%x,STUB:]]
# BRIDGE-NEXT: ret
# RUN: llvm-objdump -d --no-show-raw-insn --start-address=0x600000 \
# RUN:   --disassemble-symbols=callee %t.bridge | FileCheck %s --check-prefix=CALLEE
# CALLEE-LABEL: <callee>:
# CALLEE-NEXT: bl
# CALLEE-NEXT: ret

## Four 80 MiB bodies require multiple B hops in both directions. The extra
## hops are allocated at safe boundaries and are included in retained bytes.
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux -defsym=CHAIN=1 \
# RUN:   %t/bridge.s -o %t.chain.o
# RUN: ld.lld --emit-relocs -e _start %t.chain.o -o %t.chain.exe
# RUN: llvm-bolt %t.chain.exe -o %t.chain --turn-off-stubs --lite=0 \
# RUN:   --print-stub-stats --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=CHAIN
# CHAIN: JITLink compaction: {{.*}}stub-bytes=24 fallback=0
# CHAIN: JITLink stubs: {{.*}}short-adrp=0 long-mov=0 {{.*}}direct-b=6 retained=6 final-bytes=24
# RUN: %python %p/Inputs/check-branch-chain.py llvm-nm %t.chain 3

#--- short.s
.text
.globl _start
.type _start, %function
_start:
  b callee
.size _start, .-_start
.globl callee
.type callee, %function
callee:
  ret
.size callee, .-callee

#--- bridge.s
.text
.globl _start
.type _start, %function
_start:
  .ifdef CHAIN
  b callee
  .else
  bl callee
  .endif
  ret
  .space 0x5000000
.size _start, .-_start
.globl middle
.type middle, %function
middle:
  ret
  .space 0x5000000
.size middle, .-middle
  .ifdef CHAIN
.globl middle2
.type middle2, %function
middle2:
  ret
  .space 0x5000000
.size middle2, .-middle2
.globl middle3
.type middle3, %function
middle3:
  ret
  .space 0x5000000
.size middle3, .-middle3
  .endif
.globl callee
.type callee, %function
callee:
  .ifdef CHAIN
  b _start
  .else
  bl _start
  .endif
  ret
.size callee, .-callee
