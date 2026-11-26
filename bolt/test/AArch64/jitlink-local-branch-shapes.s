## Reuse a reachable B without moving it or its metadata, and swap a
## conditional/B pair when the other destination fits a short inversion.
# REQUIRES: system-linux
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %t/reuse.s -o %t.o
# RUN: ld.lld --emit-relocs -e reuse %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --turn-off-stubs --lite=0 --keep-nops \
# RUN:   --reorder-blocks=reverse
# RUN: llvm-objdump -d --no-show-raw-insn --disassemble-symbols=reuse %t.bolt \
# RUN:   | FileCheck %s --check-prefix=REUSE
# REUSE-LABEL: <reuse>:
# REUSE-NEXT: tbz w0, #0x0, 0x[[#%x,TRAMP:]]
# REUSE-NEXT: b 0x[[#%x,PAD:]]
# REUSE-NEXT: {{0*}}[[#TRAMP]]: b 0x[[#%x,FAR:]]
# REUSE-NEXT: {{0*}}[[#PAD]]: nop
# REUSE: {{0*}}[[#FAR]]: ret

# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %t/swap.s -o %t.swap.o
# RUN: ld.lld --emit-relocs -e swap %t.swap.o -o %t.swap
# RUN: llvm-bolt %t.swap -o %t.swap.bolt --turn-off-stubs --lite=0 --keep-nops \
# RUN:   --reorder-blocks=reverse
# RUN: llvm-objdump -d --no-show-raw-insn --disassemble-symbols=swap %t.swap.bolt \
# RUN:   | FileCheck %s --check-prefix=SWAP
## Updating DWARF must preserve the same short inversion.
# RUN: llvm-bolt %t.swap -o %t.swap.debug.bolt --turn-off-stubs --lite=0 \
# RUN:   --keep-nops --reorder-blocks=reverse --update-debug-sections
# RUN: llvm-objdump -d --no-show-raw-insn --disassemble-symbols=swap %t.swap.debug.bolt \
# RUN:   | FileCheck %s --check-prefix=SWAP
# SWAP-LABEL: <swap>:
# SWAP-NEXT: b.ne 0x[[#%x,NEAR:]]
# SWAP-NEXT: b 0x[[#%x,FAR:]]
# SWAP-NEXT: ret
# SWAP-NEXT: {{0*}}[[#NEAR]]: mov x0, #0x1
# SWAP: {{0*}}[[#FAR]]: b

## A zero-count taken edge to another fragment keeps its original polarity
## and hot fall-through. The outlined B must use the source's CFA/signed-RA
## row, even though the hot epilogue restores both before the fragment ends.
## GNU_args_size is not restored by restore_state and must be emitted explicitly.
## dwarfdump currently prints this GNU opcode without a name on AArch64.
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux -mattr=+pauth \
# RUN:   %t/cold.s -o %t.cold.o
# RUN: ld.lld --emit-relocs -e cold %t.cold.o -o %t.cold
# RUN: link_fdata %s %t.cold %t.fdata
# RUN: llvm-strip --strip-unneeded %t.cold
# RUN: llvm-bolt %t.cold -o %t.cold.bolt --turn-off-stubs --lite=0 \
# RUN:   --data=%t.fdata --split-functions --split-strategy=profile2
# RUN: llvm-objdump -d --no-show-raw-insn --mattr=+pauth \
# RUN:   --disassemble-symbols=cold,cold.cold.0 %t.cold.bolt > %t.dump
# RUN: llvm-dwarfdump --eh-frame %t.cold.bolt >> %t.dump
# RUN: FileCheck %s --input-file=%t.dump --check-prefix=COLD
# COLD: [[#%x,START:]] <cold>:
# COLD-NEXT: paciasp
# COLD-NEXT: sub sp, sp, #0x10
# COLD-NEXT: tbz w0, #0x0, 0x[[#%x,THUNK:]]
# COLD-NEXT: sub sp, sp, #0x10
# COLD-NEXT: tbz w1, #0x0, 0x[[#%x,THUNK2:]]
# COLD-NEXT: add sp, sp, #0x20
# COLD-NEXT: autiasp
# COLD-NEXT: ret
# COLD-NEXT: {{0*}}[[#THUNK2]]: b {{.*}} <cold.cold.0{{.*}}>
# COLD-NEXT: {{0*}}[[#THUNK]]: b {{.*}} <cold.cold.0>
# COLD: FDE cie={{.*}} pc={{0*}}[[#START]]...{{0*}}[[#THUNK + 4]]
# COLD: {{^ *(DW_CFA_GNU_args_size)?}}: +16
# COLD: DW_CFA_remember_state:
# COLD: {{^ *(DW_CFA_GNU_args_size)?}}: +32
# COLD: DW_CFA_remember_state:
# COLD: {{^ *(DW_CFA_GNU_args_size)?}}: +0
# COLD: DW_CFA_restore_state:
# COLD-NEXT: {{^ *(DW_CFA_GNU_args_size)?}}: +32
# COLD: DW_CFA_restore_state:
# COLD-NEXT: {{^ *(DW_CFA_GNU_args_size)?}}: +16
# COLD: 0x[[#START + 8]]: CFA=WSP+16: reg34=1
# COLD: 0x[[#START + 16]]: CFA=WSP+32: reg34=1
# COLD: 0x[[#START + 24]]: CFA=WSP: reg34=1
# COLD: 0x[[#START + 28]]: CFA=WSP: reg34=0
# COLD: 0x[[#THUNK2]]: CFA=WSP+32: reg34=1
# COLD: 0x[[#THUNK]]: CFA=WSP+16: reg34=1

## In reverse source order the second trampoline is at the CMPBR limit.
## Charging an append position instead would spuriously reject this fragment.
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux -mattr=+cmpbr \
# RUN:   %t/end-limit.s -o %t.limit.o
# RUN: ld.lld --emit-relocs -e end_limit %t.limit.o -o %t.limit
# RUN: llvm-bolt %t.limit -o %t.limit.bolt --turn-off-stubs --lite=0 --keep-nops
# RUN: llvm-objdump -d --no-show-raw-insn --mattr=+cmpbr \
# RUN:   --disassemble-symbols=end_limit %t.limit.bolt | FileCheck %s --check-prefix=LIMIT
# LIMIT: [[#%x,LIMIT:]] <end_limit>:
# LIMIT-NEXT: b.eq 0x[[#LIMIT + 1028]]
# LIMIT-NEXT: cbgt x0, #0x3f, 0x[[#LIMIT + 1024]]
# LIMIT-NEXT: br x2
# LIMIT: {{0*}}[[#LIMIT + 1024]]: b {{.*}} <callee2>
# LIMIT-NEXT: {{0*}}[[#LIMIT + 1028]]: b {{.*}} <callee1>

## Like LongJmp, reuse a B introduced by an earlier inline relaxation too.
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux -mattr=+cmpbr \
# RUN:   %t/inline-reuse.s -o %t.inline.o
# RUN: ld.lld --emit-relocs -e inline_reuse %t.inline.o -o %t.inline
# RUN: llvm-bolt %t.inline -o %t.inline.bolt --turn-off-stubs --lite=0 \
# RUN:   --keep-nops --reorder-blocks=reverse
# RUN: llvm-objdump -d --no-show-raw-insn --mattr=+cmpbr \
# RUN:   --disassemble-symbols=inline_reuse %t.inline.bolt | FileCheck %s --check-prefix=INLINE
# INLINE-LABEL: <inline_reuse>:
# INLINE-NEXT: cmp x2, #0x0
# INLINE-NEXT: cbgt x0, #0x3f, 0x[[#%x,TAKEN:]]
# INLINE-NEXT: b 0x[[#%x,END:]]
# INLINE-NEXT: {{0*}}[[#TAKEN]]: b
# INLINE-NEXT: {{0*}}[[#END]]: b
# INLINE-NEXT: ret
# INLINE-NEXT: tbz w1, #0x0, 0x[[#TAKEN]]

#--- reuse.s
.text
.globl reuse
.type reuse, %function
reuse:
  tbz w0, #0, .Lfar
  b .Lpadding
.Lfar:
  ret
.Lpadding:
  .rept 8192
  nop
  .endr
  b .Ltrampoline
.Ltrampoline:
  b .Lfar
.size reuse, .-reuse
.reloc 0, R_AARCH64_NONE

#--- end-limit.s
.text
.globl callee1, callee2, end_limit
.type callee1, %function
callee1:
  ret
.size callee1, .-callee1
.type callee2, %function
callee2:
.Lcallee2:
  ret
.size callee2, .-callee2
.type end_limit, %function
end_limit:
  b.eq callee1
  cbgt x0, #63, .Lcallee2
  br x2
  .rept 252
  nop
  .endr
  ret
.size end_limit, .-end_limit

#--- swap.s
.text
.globl swap
.type swap, %function
swap:
  b.eq .Lfar
  b .Lnear
.Lfar:
  b .Lpadding
.Lpadding:
  .rept 262144
  nop
  .endr
  b .Lnear
.Lnear:
  mov x0, #1
  b .Lother
.Lother:
  ret
.size swap, .-swap
.reloc 0, R_AARCH64_NONE

#--- cold.s
.text
.globl cold
.type cold, %function
cold:
# FDATA: 0 [unknown] 0 1 cold 0 0 100
.cfi_startproc
  paciasp
.cfi_negate_ra_state
  sub sp, sp, #16
.cfi_def_cfa_offset 16
.cfi_escape 0x2e, 16
.branch:
# FDATA: 1 cold #.branch# 1 cold #.taken# 0 0
# FDATA: 1 cold #.branch# 1 cold #.hot# 0 100
  tbz w0, #0, .taken
.hot:
  sub sp, sp, #16
.cfi_def_cfa_offset 32
.cfi_escape 0x2e, 32
.branch2:
# FDATA: 1 cold #.branch2# 1 cold #.taken2# 0 0
# FDATA: 1 cold #.branch2# 1 cold #.hot2# 0 100
  tbz w1, #0, .taken2
.hot2:
  add sp, sp, #32
.cfi_def_cfa_offset 0
.cfi_escape 0x2e, 0
  autiasp
.cfi_negate_ra_state
  ret
.taken:
.cfi_def_cfa_offset 16
.cfi_escape 0x2e, 16
.cfi_negate_ra_state
  .rept 20
  mov x0, #42
  .endr
  add sp, sp, #16
.cfi_def_cfa_offset 0
  autiasp
.cfi_negate_ra_state
  ret
.taken2:
.cfi_def_cfa_offset 32
.cfi_escape 0x2e, 32
.cfi_negate_ra_state
  .rept 20
  mov x0, #43
  .endr
  add sp, sp, #32
.cfi_def_cfa_offset 0
  autiasp
.cfi_negate_ra_state
  ret
.cfi_endproc
.size cold, .-cold
.reloc 0, R_AARCH64_NONE

#--- inline-reuse.s
.text
.globl inline_reuse
.type inline_reuse, %function
inline_reuse:
  cmp x2, #0
  cbgt x0, #63, .Lfar
  b .Lpadding
.Lfar:
  csel x0, x0, x1, eq
  cbz x3, .Llast
  ret
.Lpadding:
  .rept 8189
  nop
  .endr
.Llast:
  tbz w1, #0, .Lfar
  ret
.size inline_reuse, .-inline_reuse
.reloc 0, R_AARCH64_NONE
