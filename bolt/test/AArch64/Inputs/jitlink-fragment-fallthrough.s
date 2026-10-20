## Shared input for guard, FDE and far-stub checks.
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

## Fall-through without a direct branch edge to reserve a slot for.
.globl indirect
.type indirect, %function
indirect:
  blr x0
.size indirect, .-indirect

## Fall-through at the end of the output section.
.globl last
.type last, %function
last:
  .cfi_startproc
  bl callee
  .cfi_endproc
.size last, .-last
