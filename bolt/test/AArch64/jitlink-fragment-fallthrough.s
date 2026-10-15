## Keep a fall-through guard inside the source FDE and branch over the reserved
## stub space, including alignment padding before the next fragment.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %s -o %t.o
# RUN: ld.lld --emit-relocs -e _start %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --turn-off-stubs --lite=0 \
# RUN:   --align-functions=16 --align-functions-max-bytes=15 \
# RUN:   --verify-branch26-range | FileCheck %s --check-prefix=LOG
# RUN: llvm-objdump -d --no-show-raw-insn --section=.text %t.bolt > %t.dump
# RUN: llvm-dwarfdump --eh-frame %t.bolt >> %t.dump
# RUN: FileCheck %s --input-file=%t.dump

## Moving callee out of range also verifies that inter-fragment b/bl edges
## survive MC emission and that the guard skips a materialized stub.
# RUN: llvm-bolt %t.exe -o %t.far.bolt --turn-off-stubs --lite=0 \
# RUN:   --align-functions=16 --align-functions-max-bytes=15 \
# RUN:   --pad-funcs-before=callee:134217984 --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=FAR-LOG
# RUN: llvm-objdump -d --no-show-raw-insn --disassemble-symbols=_start \
# RUN:   %t.far.bolt | FileCheck %s --check-prefix=FAR

## Three direct branches reserve three slots; the three guards reserve none.
# LOG-COUNT-3: BOLT-INFO: created 1 slots after block for stubs
# LOG: BOLT-INFO: 6 branches detected beforefixups.
# LOG-NEXT: BOLT-INFO: 0 branch destinations are unreachable.
# FAR-LOG: BOLT-INFO: short jump stub was materialized.
# FAR-LOG: BOLT-INFO: 0 branch destinations are unreachable.

# CHECK: [[#%x,START:]] <_start>:
# CHECK-NEXT: sub sp, sp, #0x10
# CHECK-NEXT: str x30, [sp, #0x8]
# CHECK-NEXT: bl {{.*}} <callee>
# CHECK-NEXT: b 0x[[#%x,CONT:]] <continuation>
# CHECK-COUNT-5: nop
# CHECK: {{0*}}[[#CONT]] <continuation>:
# CHECK: b {{.*}} <callee>
# CHECK-LABEL: <indirect>:
# CHECK-NEXT: blr x0
# CHECK-NEXT: b {{.*}} <last>
# CHECK: [[#%x,LAST:]] <last>:
# CHECK-NEXT: bl {{.*}} <callee>
# CHECK-NEXT: b 0x[[#LAST + 28]]
# CHECK-COUNT-5: nop

## The first FDE covers all four instructions, including the guard, but none
## of the stub space. Its final CFA/LR rules also apply to the guard.
# CHECK: FDE cie={{.*}} pc={{0*}}[[#START]]...{{0*}}[[#START + 16]]
# CHECK: 0x[[#START + 8]]: CFA=WSP+16: W30=[CFA-8]
# CHECK: FDE cie={{.*}} pc={{0*}}[[#LAST]]...{{0*}}[[#LAST + 8]]

# FAR-LABEL: <_start>:
# FAR-NEXT: sub sp, sp, #0x10
# FAR-NEXT: str x30, [sp, #0x8]
# FAR-NEXT: bl 0x[[#%x,STUB:]]
# FAR-NEXT: b {{.*}} <continuation>
# FAR-NEXT: {{0*}}[[#STUB]]: adrp x16,
# FAR-NEXT: add x16, x16,
# FAR-NEXT: br x16
# FAR-NEXT: nop
# FAR-NEXT: nop

.text
.globl _start
.type _start, %function
_start:
  .cfi_startproc
  sub sp, sp, #16
  .cfi_def_cfa_offset 16
  str x30, [sp, #8]
  .cfi_offset w30, -8
  bl callee
  .cfi_endproc
.size _start, .-_start

.p2align 4
.globl continuation
.type continuation, %function
continuation:
  .cfi_startproc
  .cfi_def_cfa_offset 16
  .cfi_offset w30, -8
  ldr x30, [sp, #8]
  add sp, sp, #16
  .cfi_def_cfa_offset 0
  .cfi_restore w30
  b callee
  .cfi_endproc
.size continuation, .-continuation

.globl callee
.type callee, %function
callee:
  ret
.size callee, .-callee

.globl indirect
.type indirect, %function
indirect:
  blr x0
.size indirect, .-indirect

.globl last
.type last, %function
last:
  .cfi_startproc
  bl callee
  .cfi_endproc
.size last, .-last
