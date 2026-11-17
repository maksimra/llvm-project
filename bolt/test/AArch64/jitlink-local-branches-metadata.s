## Late expansion must preserve CFI, PAC state, LSDA call-site/landing-pad
## addresses, DIE ranges and line-table rows. Reordering moves the taken block
## beyond TBZ range, so emission adds an instruction before all later labels.

# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux -mattr=+pauth \
# RUN:   -dwarf-version=4 %s -o %t.o
# RUN: ld.lld --emit-relocs -e metadata %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --turn-off-stubs --lite=0 --keep-nops \
# RUN:   --reorder-blocks=reverse --update-debug-sections --verify-branch26-range
# RUN: llvm-objdump -d --no-show-raw-insn --mattr=+pauth \
# RUN:   --disassemble-symbols=metadata %t.bolt > %t.dump
# RUN: llvm-dwarfdump --debug-info --eh-frame --debug-line %t.bolt >> %t.dump
# RUN: FileCheck %s --input-file=%t.dump
# RUN: llvm-dwarfdump --verify %t.bolt | FileCheck %s --check-prefix=VERIFY
# RUN: %python %p/Inputs/check-local-branches-lsda.py %t.bolt %t.dump

# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux -mattr=+pauth \
# RUN:   -dwarf-version=5 --defsym=DWARF5=1 %s -o %t.5.o
# RUN: ld.lld --emit-relocs -e metadata %t.5.o -o %t.5.exe
# RUN: llvm-bolt %t.5.exe -o %t.5.bolt --turn-off-stubs --lite=0 --keep-nops \
# RUN:   --reorder-blocks=reverse --update-debug-sections --verify-branch26-range
# RUN: llvm-objdump -d --no-show-raw-insn --mattr=+pauth \
# RUN:   --disassemble-symbols=metadata %t.5.bolt > %t.5.dump
# RUN: llvm-dwarfdump --debug-info --eh-frame --debug-line %t.5.bolt >> %t.5.dump
# RUN: FileCheck %s --input-file=%t.5.dump
# RUN: llvm-dwarfdump --verify %t.5.bolt | FileCheck %s --check-prefix=VERIFY
# RUN: %python %p/Inputs/check-local-branches-lsda.py %t.5.bolt %t.5.dump

# VERIFY: No errors.

# CHECK: [[#%x,START:]] <metadata>:
# CHECK-NEXT: paciasp
# CHECK-NEXT: stp x29, x30, [sp, #-0x10]!
# CHECK-NEXT: tbnz w0, #0x0, 0x[[#%x,FALL:]]
# CHECK-NEXT: b 0x[[#%x,TAKEN:]]
# CHECK-NEXT: {{0*}}[[#FALL]]: b 0x[[#%x,CALL:]]
# CHECK-NEXT: [[#%x,RETURN:]]: ldp x29, x30, [sp], #0x10
# CHECK-NEXT: autiasp
# CHECK-NEXT: ret
# CHECK-NEXT: [[#%x,LP:]]: mov w0, #0x2a
# CHECK: {{0*}}[[#CALL]]: bl
# CHECK-NEXT: b
# CHECK-NEXT: {{0*}}[[#TAKEN]]: mov w0, #0x1
# CHECK-NEXT: b

# CHECK: DW_TAG_subprogram
# CHECK: DW_AT_low_pc (0x{{0*}}[[#START]])
# CHECK: DW_AT_high_pc (0x{{0*}}[[#TAKEN + 8]])
# CHECK: DW_TAG_variable
# CHECK: [0x{{0*}}[[#CALL]], 0x{{0*}}[[#CALL + 4]]): DW_OP_reg8 W8
# CHECK: DW_TAG_call_site
# CHECK: DW_AT_call_return_pc (0x{{0*}}[[#CALL + 4]])

## Both added branches inherit the CFA and signed-RA state of the TBZ. The
## epilogue and landing pad retain their own rules at their shifted addresses.
# CHECK: FDE cie={{.*}} pc={{0*}}[[#START]]...{{0*}}[[#TAKEN + 8]]
# CHECK: LSDA Address:
# CHECK: 0x[[#START]]: CFA=WSP
# CHECK-NEXT: 0x[[#START + 4]]: CFA=WSP: reg34=1
# CHECK-NEXT: 0x[[#START + 8]]: CFA=WSP+16: W29=[CFA-16], W30=[CFA-8], reg34=1
# CHECK-NEXT: 0x[[#RETURN + 4]]: CFA=WSP: reg34=1
# CHECK-NEXT: 0x[[#RETURN + 8]]: CFA=WSP: reg34=0
# CHECK-NEXT: 0x[[#LP]]: CFA=WSP+16: W29=[CFA-16], W30=[CFA-8], reg34=1

## Line rows stay at the original instructions; expansion bytes belong to line
## 2, and the sequence ends at the final fragment boundary.
# CHECK: .debug_line contents:
# CHECK: 0x{{0*}}[[#START]] 1 0
# CHECK-NEXT: 0x{{0*}}[[#START + 8]] 2 0
# CHECK-NEXT: 0x{{0*}}[[#RETURN]] 7 0
# CHECK-NEXT: 0x{{0*}}[[#LP]] 6 0
# CHECK: 0x{{0*}}[[#CALL]] 3 0
# CHECK-NEXT: 0x{{0*}}[[#TAKEN]] 4 0
# CHECK-NEXT: 0x{{0*}}[[#TAKEN + 8]] 4 0 {{.*}} end_sequence

.text
.ifdef DWARF5
.file 0 "local-branches.c"
.else
.file 1 "local-branches.c"
.endif

.macro source_line number
.ifdef DWARF5
  .loc 0 \number 0
.else
  .loc 1 \number 0
.endif
.endm
.globl metadata
.type metadata, %function
metadata:
.Lbegin:
  .cfi_startproc
  .cfi_personality 0, personality
  .cfi_lsda 0, .LLSDA
  source_line 1
  paciasp
  .cfi_negate_ra_state
  stp x29, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  .cfi_offset w29, -16
  .cfi_offset w30, -8
  source_line 2
  tbz w0, #0, .Ltaken
  b .Lcall
.Ltaken:
  source_line 4
  mov w0, #1
  b .Lpadding
.Lcall:
  source_line 3
  bl may_throw
.Lcallend:
.Lpadding:
  source_line 5
  .rept 8192
  nop
  .endr
  b .Lreturn
.Llanding:
  source_line 6
  mov w0, #42
  b .Lreturn
.Lreturn:
  source_line 7
  ldp x29, x30, [sp], #16
  .cfi_def_cfa_offset 0
  .cfi_restore w29
  .cfi_restore w30
  autiasp
  .cfi_negate_ra_state
  ret
  .cfi_endproc
.Lend:
.size metadata, .-metadata

.globl may_throw, personality
.type may_throw, %function
may_throw:
personality:
  ret
.size may_throw, .-may_throw

.section .gcc_except_table,"a",@progbits
.LLSDA:
  .byte 255, 255, 1
  .uleb128 .LCSEnd-.LCSStart
.LCSStart:
  .uleb128 .Lcall-metadata
  .uleb128 .Lcallend-.Lcall
  .uleb128 .Llanding-metadata
  .uleb128 0
.LCSEnd:

.section .debug_abbrev,"",@progbits
.Labbrev:
  .byte 1, 17, 1
  .byte 3, 8
  .byte 16, 23
  .byte 17, 1
  .byte 18, 6
  .byte 0, 0
  .byte 2, 46, 1
  .byte 3, 8
  .byte 17, 1
  .byte 18, 6
  .byte 64, 24
  .byte 122, 25
  .byte 0, 0
  .byte 3, 52, 0
  .byte 3, 8
  .byte 2, 23
  .byte 0, 0
  .byte 4, 72, 0
  .byte 125, 1
  .byte 0, 0
  .byte 0

.section .debug_info,"",@progbits
  .long .LDIEEnd-.LDIEStart
.LDIEStart:
.ifdef DWARF5
  .short 5
  .byte 1, 8
  .long .Labbrev
.else
  .short 4
  .long .Labbrev
  .byte 8
.endif
  .byte 1
  .asciz "local-branches.c"
  .long .Lline
  .quad .Lbegin
  .long .Lend-.Lbegin
  .byte 2
  .asciz "metadata"
  .quad .Lbegin
  .long .Lend-.Lbegin
  .uleb128 1
  .byte 156
  .byte 3
  .asciz "x"
  .long .Lloc
  .byte 4
  .quad .Lcallend
  .byte 0
  .byte 0
.LDIEEnd:

.ifdef DWARF5
.section .debug_loclists,"",@progbits
  .long .Llocend-.Llocheader
.Llocheader:
  .short 5
  .byte 8, 0
  .long 0
.Lloc:
  .byte 8
  .quad .Lbegin
  .uleb128 .Lcallend-.Lbegin
  .uleb128 1
  .byte 88
  .byte 0
.Llocend:
.else
.section .debug_loc,"",@progbits
.Lloc:
  .quad 0, .Lcallend-.Lbegin
  .short 1
  .byte 88
  .quad 0, 0
.endif

.section .debug_line,"",@progbits
.Lline:
