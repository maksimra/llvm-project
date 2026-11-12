## Non-simple functions retain their original instruction offsets. Their tail
## trampolines need the source branch's unwind rules, not the final body's rules.

# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %s -o %t.o
# RUN: ld.lld --emit-relocs -e caller %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --turn-off-stubs --lite=0 \
# RUN:   --funcs=caller,fallthrough,continuation,pac_caller \
# RUN:   --custom-allocation-vma=0x40000000 \
# RUN:   --use-gnu-stack --verify-branch26-range
# RUN: llvm-objdump -d --no-show-raw-insn --section=.text %t.bolt > %t.dump
# RUN: llvm-dwarfdump --eh-frame %t.bolt >> %t.dump
# RUN: FileCheck %s --input-file=%t.dump
# RUN: llvm-dwarfdump --verify %t.bolt | FileCheck %s --check-prefix=VERIFY

# CHECK-LABEL: <caller>:
# CHECK-NEXT: cbz x0, 0x[[#%x,ENTRY_TAIL:]]
# CHECK-NEXT: cbz x2, 0x[[#%x,OTHER_ENTRY_TAIL:]]
# CHECK-NEXT: b
# CHECK: cbz x3, 0x[[#%x,FRAME_TAIL:]]
# CHECK-NEXT: cbz x4, 0x[[#%x,EXPR_TAIL:]]
# CHECK-NEXT: mov x9, sp
# CHECK-NEXT: br x1
## The constant island remains before the pool. Trampolines consume the saved
## states in reverse order, sharing one snapshot for the two entry branches.
# CHECK: .word 0x12345678
# CHECK: b 0x[[#%x,CALLER_END:]]
# CHECK-NEXT: {{0*}}[[#EXPR_TAIL]]: b
# CHECK-NEXT: {{0*}}[[#FRAME_TAIL]]: b
# CHECK-NEXT: {{0*}}[[#OTHER_ENTRY_TAIL]]: b
# CHECK-NEXT: {{0*}}[[#ENTRY_TAIL]]: b

# CHECK-LABEL: <fallthrough>:
# CHECK-NEXT: cbz x0, 0x[[#%x,FALL_TAIL:]]
# CHECK-NEXT: br x1
# CHECK-NEXT: stp x19, x30, [sp, #-0x10]!
# CHECK-NEXT: bl
# CHECK-NEXT: b 0x[[#%x,FALL_CONT:]]
# CHECK-NEXT: {{0*}}[[#FALL_TAIL]]: b
# CHECK-NEXT: {{0*}}[[#FALL_CONT]]: b

# CHECK-LABEL: <pac_caller>:
# CHECK-NEXT: cbz x0, 0x[[#%x,UNSIGNED_TAIL:]]
# CHECK-NEXT: paciasp
# CHECK-NEXT: cbz x2, 0x[[#%x,SIGNED_TAIL:]]
# CHECK-NEXT: br x1
# CHECK-NEXT: b 0x[[#%x,PAC_END:]]
# CHECK-NEXT: {{0*}}[[#SIGNED_TAIL]]: b
# CHECK-NEXT: {{0*}}[[#UNSIGNED_TAIL]]: b

## Check the interpreted rules, including restoration of the complete register
## state and a CFA expression. Function sizes alone cannot detect this bug.
# CHECK: FDE cie={{.*}} pc={{.*}}...{{0*}}[[#CALLER_END]]
# CHECK: 0x[[#EXPR_TAIL]]: CFA=DW_OP_breg31 WSP+16: W19=[CFA-16], W30=[CFA-8]
# CHECK-NEXT: 0x[[#FRAME_TAIL]]: CFA=WSP+16: W19=[CFA-16], W30=[CFA-8]
# CHECK-NEXT: 0x[[#OTHER_ENTRY_TAIL]]: CFA=WSP{{$}}
# CHECK: FDE cie={{.*}} pc={{.*}}...{{0*}}[[#FALL_CONT + 4]]
# CHECK: 0x[[#FALL_TAIL]]: CFA=WSP{{$}}
# CHECK-NEXT: 0x[[#FALL_CONT]]: CFA=WSP+16: W19=[CFA-16], W30=[CFA-8]
# CHECK: FDE cie={{.*}} pc={{.*}}...{{0*}}[[#PAC_END]]
# CHECK: 0x[[#SIGNED_TAIL]]: CFA=WSP: reg34=1
# CHECK-NEXT: 0x[[#UNSIGNED_TAIL]]: CFA=WSP{{$}}
# VERIFY: No errors.

.text
.globl caller
.type caller, %function
caller:
  .cfi_startproc
  cbz x0, callee
  cbz x2, callee
  b .Lbody
.Lexit:
  .cfi_def_cfa sp, 16
  .cfi_offset x19, -16
  .cfi_offset x30, -8
  ldp x19, x30, [sp], #16
  .cfi_def_cfa_offset 0
  .cfi_restore x19
  .cfi_restore x30
  ret
.Lbody:
  .cfi_def_cfa sp, 0
  stp x19, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  .cfi_offset x19, -16
  .cfi_offset x30, -8
  cbz x3, framed_callee
  # DW_CFA_def_cfa_expression: DW_OP_breg31 +16.
  .cfi_escape 0x0f, 0x02, 0x8f, 0x10
  cbz x4, framed_callee
  mov x9, sp
  .cfi_def_cfa x9, 16
  br x1
  .word 0x12345678
  .cfi_endproc
.size caller, .-caller

.globl fallthrough
.type fallthrough, %function
fallthrough:
  .cfi_startproc
  cbz x0, callee
  br x1
.Lfall_body:
  stp x19, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  .cfi_offset x19, -16
  .cfi_offset x30, -8
  bl helper
  .cfi_endproc
.size fallthrough, .-fallthrough

.globl continuation
.type continuation, %function
continuation:
  .cfi_startproc
  .cfi_def_cfa_offset 16
  .cfi_offset x19, -16
  .cfi_offset x30, -8
  ldp x19, x30, [sp], #16
  .cfi_def_cfa_offset 0
  .cfi_restore x19
  .cfi_restore x30
  ret
  .cfi_endproc
.size continuation, .-continuation

.globl callee
.type callee, %function
callee:
  .cfi_startproc
  ret
  .cfi_endproc
.size callee, .-callee

.globl framed_callee
.type framed_callee, %function
framed_callee:
  .cfi_startproc
  .cfi_def_cfa_offset 16
  .cfi_offset x19, -16
  .cfi_offset x30, -8
  ldp x19, x30, [sp], #16
  .cfi_def_cfa_offset 0
  .cfi_restore x19
  .cfi_restore x30
  ret
  .cfi_endproc
.size framed_callee, .-framed_callee

.globl helper
.type helper, %function
helper:
  .cfi_startproc
  ret
  .cfi_endproc
.size helper, .-helper

.globl pac_caller
.type pac_caller, %function
pac_caller:
  .cfi_startproc
  cbz x0, callee
  paciasp
  .cfi_negate_ra_state
  cbz x2, signed_callee
  br x1
  .cfi_endproc
.size pac_caller, .-pac_caller

.globl signed_callee
.type signed_callee, %function
signed_callee:
  .cfi_startproc
  .cfi_negate_ra_state
  autiasp
  .cfi_negate_ra_state
  ret
  .cfi_endproc
.size signed_callee, .-signed_callee
.reloc 0, R_AARCH64_NONE
