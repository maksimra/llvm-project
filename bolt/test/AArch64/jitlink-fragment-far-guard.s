## A synthetic fall-through guard must be relaxed when padding puts its
## continuation outside Branch26 range, even if there are no other branches.
## The guard can share both a reservation and a materialized stub with a call.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %s -o %t.o
# RUN: ld.lld --emit-relocs -e first %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --turn-off-stubs --lite=0 --split-functions \
# RUN:   --pad-funcs-before=second:134217984 --print-stub-stats --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=UNIQUE-STATS
# RUN: llvm-objdump -d --no-show-raw-insn --disassemble-symbols=first,second \
# RUN:   %t.bolt | FileCheck %s --check-prefixes=UNIQUE,CHECK
# UNIQUE-STATS: BOLT-INFO: JITLink stubs: slots=1 unused=0 short-adrp=1 long-mov=0 shared=0

# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux --defsym=SHARE=1 \
# RUN:   %s -o %t.shared.o
# RUN: ld.lld --emit-relocs -e first %t.shared.o -o %t.shared.exe
# RUN: llvm-bolt %t.shared.exe -o %t.shared.bolt --turn-off-stubs --lite=0 \
# RUN:   --split-functions --pad-funcs-before=second:134217984 --print-stub-stats \
# RUN:   --verify-branch26-range | FileCheck %s --check-prefix=SHARED-STATS
# RUN: llvm-objdump -d --no-show-raw-insn --disassemble-symbols=first,second \
# RUN:   %t.shared.bolt | FileCheck %s --check-prefixes=SHARED,CHECK
# SHARED-STATS: BOLT-INFO: JITLink stubs: slots=1 unused=0 short-adrp=1 long-mov=0 shared=1

# CHECK-LABEL: <first>:
# SHARED-NEXT: bl 0x[[#%x,STUB:]]
# CHECK-NEXT: blr x0
# UNIQUE-NEXT: b 0x[[#%x,STUB:]]
# SHARED-NEXT: b 0x[[#STUB]]
# CHECK-NEXT: {{0*}}[[#STUB]]: adrp x16, 0x[[#%x,DEST:]]
# CHECK-NEXT: add x16, x16, #0x[[#%x,DEST_LO:]]
# CHECK-NEXT: br x16
# CHECK-NEXT: nop
# CHECK-NEXT: nop
# CHECK: {{0*}}[[#DEST + DEST_LO]] <second>:
# CHECK-NEXT: ret

.text
.globl first
.type first, %function
first:
.ifdef SHARE
  bl second
.endif
  blr x0
.size first, .-first

.globl second
.type second, %function
second:
  ret
.size second, .-second

.reloc 0, R_AARCH64_NONE
