## A fall-through guard skips reserved stub space and alignment padding.
## Also cover a guard without other branches and fall-through at section end.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux \
# RUN:   %p/Inputs/jitlink-fragment-fallthrough.s -o %t.o
# RUN: ld.lld --emit-relocs -e _start %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --turn-off-stubs --lite=0 \
# RUN:   --align-functions=16 --verify-branch26-range
# RUN: llvm-objdump -d --no-show-raw-insn --section=.text %t.bolt \
# RUN:   | FileCheck %s

## The guard lands on continuation itself, past both reserved slots and padding.
# CHECK-LABEL: <_start>:
# CHECK-NEXT: sub sp, sp, #0x10
# CHECK-NEXT: str x30, [sp, #0x8]
# CHECK-NEXT: bl {{.*}} <callee>
# CHECK-NEXT: b 0x[[#%x,CONT:]] <continuation>
# CHECK-COUNT-10: nop
# CHECK: {{0*}}[[#CONT]] <continuation>:
# CHECK-NEXT: ldr x30, [sp, #0x8]
# CHECK-NEXT: add sp, sp, #0x10
# CHECK-NEXT: b {{.*}} <callee>

## An indirect call reserves a slot for its guard too.
# CHECK-LABEL: <indirect>:
# CHECK-NEXT: blr x0
# CHECK-NEXT: b 0x[[#%x,LAST:]] <last>
# CHECK-COUNT-5: nop

## At section end, the guard skips the call and guard reservations.
# CHECK: {{0*}}[[#LAST]] <last>:
# CHECK-NEXT: bl {{.*}} <callee>
# CHECK-NEXT: b 0x[[#LAST + 48]]
# CHECK-COUNT-10: nop
