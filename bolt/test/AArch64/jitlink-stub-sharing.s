## Repeated b/bl destinations share reservations within and across fragments.
## Different addends name different destinations and must use different stubs.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %s -o %t.o
# RUN: ld.lld --emit-relocs -e _start %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.near --turn-off-stubs --lite=0 \
# RUN:   --align-functions=16 --print-stub-stats \
# RUN:   | FileCheck %s --check-prefix=NEAR
# NEAR: BOLT-INFO: JITLink stubs: slots=0 unused=0 short-adrp=0 long-mov=0 shared=0

## Lookahead proves all destinations reachable, so no slots are reserved.
# RUN: llvm-objdump -d --no-show-raw-insn --section=.text %t.near \
# RUN:   | FileCheck %s --check-prefix=NEAR-CODE
# NEAR-CODE: [[#%x,START:]] <_start>:
# NEAR-CODE-NEXT: bl {{.*}} <callee>
# NEAR-CODE-NEXT: bl {{.*}} <callee>
# NEAR-CODE-NEXT: bl {{.*}} <callee{{.*}}>
# NEAR-CODE-NEXT: b {{.*}} <callee>
# NEAR-CODE: {{0*}}[[#START + 16]] <second>:
# NEAR-CODE-NEXT: bl {{.*}}
# NEAR-CODE-NEXT: b {{.*}} <callee>
# NEAR-CODE: {{0*}}[[#START + 24]] <callee>:

# RUN: llvm-bolt %t.exe -o %t.far --turn-off-stubs --lite=0 \
# RUN:   --align-functions=16 --pad-funcs-before=callee:134217984 \
# RUN:   --print-stub-stats --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=FAR
# FAR: BOLT-INFO: JITLink stubs: slots=2 unused=0 short-adrp=2 long-mov=0 shared=4
# RUN: llvm-objdump -d --no-show-raw-insn --disassemble-symbols=_start,second \
# RUN:   %t.far | FileCheck %s --check-prefixes=SHARED,SHORT

## Sharing short stubs must also work in position-independent code.
# RUN: ld.lld --emit-relocs -pie -e _start %t.o -o %t.pie
# RUN: llvm-bolt %t.pie -o %t.pie.bolt --turn-off-stubs --lite=0 \
# RUN:   --align-functions=16 --pad-funcs-before=callee:134217984 \
# RUN:   --print-stub-stats --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=FAR
# RUN: llvm-objdump -d --no-show-raw-insn --disassemble-symbols=_start,second \
# RUN:   %t.pie.bolt | FileCheck %s --check-prefixes=SHARED,SHORT

## Keep callee at its original address and move callers beyond ADRP range.
## The two shared slots must each materialize a single MOVZ/MOVK/BR sequence.
# RUN: llvm-bolt %t.exe -o %t.long --turn-off-stubs --lite=0 \
# RUN:   --align-functions=16 --skip-funcs=callee --use-gnu-stack \
# RUN:   --custom-allocation-vma=0x1234500000 \
# RUN:   --print-stub-stats --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=LONG-STATS
# LONG-STATS: BOLT-INFO: JITLink stubs: slots=2 unused=0 short-adrp=0 long-mov=2 shared=4
# RUN: llvm-objdump -d --no-show-raw-insn --disassemble-symbols=_start,second \
# RUN:   %t.long | FileCheck %s --check-prefixes=SHARED,LONG

# SHARED-LABEL: <_start>:
# SHARED-NEXT: bl 0x[[#%x,STUB:]]
# SHARED-NEXT: bl 0x[[#STUB]]
# SHARED-NEXT: bl 0x[[#%x,OFFSET_STUB:]]
# SHARED-NEXT: b 0x[[#STUB]]
# SHARED-LABEL: <second>:
# SHARED-NEXT: bl 0x[[#OFFSET_STUB]]
# SHARED-NEXT: b 0x[[#STUB]]
# SHORT-NEXT: {{0*}}[[#STUB]]: adrp x16,
# SHORT-NEXT: add x16, x16, #0x[[#%x,TARGET_LO:]]
# SHORT-NEXT: br x16
# SHORT-NEXT: nop
# SHORT-NEXT: nop
# SHORT-NEXT: {{0*}}[[#OFFSET_STUB]]: adrp x16,
# SHORT-NEXT: add x16, x16, #0x[[#TARGET_LO + 4]]
# SHORT-NEXT: br x16
# SHORT-NEXT: nop
# SHORT-NEXT: nop
# LONG-NEXT: {{0*}}[[#STUB]]: mov x16, #0x[[#%x,TARGET_LO:]]
# LONG-NEXT: movk x16, #0x[[#%x,TARGET_HI:]], lsl #16
# LONG-NEXT: movk x16, #0x0, lsl #32
# LONG-NEXT: movk x16, #0x0, lsl #48
# LONG-NEXT: br x16
# LONG-NEXT: {{0*}}[[#OFFSET_STUB]]: mov x16, #0x[[#TARGET_LO + 4]]
# LONG-NEXT: movk x16, #0x[[#TARGET_HI]], lsl #16
# LONG-NEXT: movk x16, #0x0, lsl #32
# LONG-NEXT: movk x16, #0x0, lsl #48
# LONG-NEXT: br x16

## A shared reservation outside Branch26 range must not be reused. Padding
## before each of second and callee leaves both callers out of target range,
## while separating second from the first pair of slots by more than 128 MiB.
# RUN: llvm-bolt %t.exe -o %t.separated --turn-off-stubs --lite=0 \
# RUN:   --align-functions=16 \
# RUN:   --pad-funcs-before=second:134217984,callee:134217984 \
# RUN:   --print-stub-stats --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=SEPARATED
# SEPARATED: BOLT-INFO: JITLink stubs: slots=4 unused=0 short-adrp=4 long-mov=0 shared=2
# RUN: llvm-objdump -d --no-show-raw-insn --disassemble-symbols=_start,second \
# RUN:   %t.separated | FileCheck %s --check-prefix=SEPARATED-CODE
# SEPARATED-CODE-LABEL: <_start>:
# SEPARATED-CODE-NEXT: bl 0x[[#%x,FIRST_STUB:]]
# SEPARATED-CODE-NEXT: bl 0x[[#FIRST_STUB]]
# SEPARATED-CODE-NEXT: bl 0x[[#FIRST_STUB + 20]]
# SEPARATED-CODE-NEXT: b 0x[[#FIRST_STUB]]
# SEPARATED-CODE: [[#%x,SECOND:]] <second>:
# SEPARATED-CODE-NEXT: bl 0x[[#SECOND + 8]]
# SEPARATED-CODE-NEXT: b 0x[[#SECOND + 28]]

## The negative Branch26 limit is inclusive. Increasing this padding by four
## bytes makes only second's last branch unable to reach its original slot.
# RUN: llvm-bolt %t.exe -o %t.boundary --turn-off-stubs --lite=0 \
# RUN:   --align-functions=16 \
# RUN:   --pad-funcs-before=second:134217684,callee:134217984 \
# RUN:   --print-stub-stats --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=FAR
# RUN: llvm-bolt %t.exe -o %t.beyond --turn-off-stubs --lite=0 \
# RUN:   --align-functions=16 \
# RUN:   --pad-funcs-before=second:134217688,callee:134217984 \
# RUN:   --print-stub-stats --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=BEYOND
# BEYOND: BOLT-INFO: JITLink stubs: slots=3 unused=0 short-adrp=3 long-mov=0 shared=3

.text
.globl _start
.type _start, %function
_start:
  bl callee
  bl callee
  bl callee+4
  b callee
.size _start, .-_start

.globl second
.type second, %function
second:
  bl callee+4
  b callee
.size second, .-second

.globl callee
.type callee, %function
callee:
  mov w0, #1
  ret
.size callee, .-callee
