## Deferred stubs use BR x16. Prepare emitted destinations before relaxation,
## preserve existing explicit/implicit landing pads, and check the exact final
## target before materializing either an ADRP or a MOV stub.

# REQUIRES: system-linux
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux -mattr=+bti,+pauth \
# RUN:   -aarch64-mark-bti-property %t/targets.s -o %t.targets.o
# RUN: ld.lld --emit-relocs -e caller %t.targets.o -o %t.targets
# RUN: llvm-bolt %t.targets -o %t.targets.bolt --turn-off-stubs --lite=0 \
# RUN:   --align-functions=4 --pad-funcs-before=plain:134217984 \
# RUN:   --print-stub-stats | FileCheck %s --check-prefix=TARGET-STATS
# TARGET-STATS: JITLink stubs: slots=5 unused=0 short-adrp=5 long-mov=0 shared=2
# RUN: llvm-objdump -d --no-show-raw-insn --mattr=+bti,+pauth \
# RUN:   --disassemble-symbols=plain,jump_pad,both_pad,pac_a,pac_b %t.targets.bolt \
# RUN:   | FileCheck %s --check-prefix=TARGETS
# TARGETS-LABEL: <plain>:
# TARGETS-NEXT: bti c
# TARGETS-NEXT: ret
# TARGETS-LABEL: <jump_pad>:
# TARGETS-NEXT: bti j
# TARGETS-NEXT: ret
# TARGETS-LABEL: <both_pad>:
# TARGETS-NEXT: bti jc
# TARGETS-NEXT: ret
# TARGETS-LABEL: <pac_a>:
# TARGETS-NEXT: paciasp
# TARGETS-NEXT: autiasp
# TARGETS-NEXT: ret
# TARGETS-LABEL: <pac_b>:
# TARGETS-NEXT: pacibsp
# TARGETS-NEXT: autibsp
# TARGETS-NEXT: ret
# RUN: llvm-readelf -n %t.targets.bolt | FileCheck %s --check-prefix=PROPERTY
# PROPERTY: Properties: aarch64 feature: BTI

## The same landing pads and ADRP stubs work in a PIE.
# RUN: ld.lld --emit-relocs -pie -e caller %t.targets.o -o %t.pie
# RUN: llvm-bolt %t.pie -o %t.pie.bolt --turn-off-stubs --lite=0 \
# RUN:   --align-functions=4 --pad-funcs-before=plain:134217984 \
# RUN:   --print-stub-stats | FileCheck %s --check-prefix=TARGET-STATS

## A skipped function cannot grow. Patch its entry NOP only when an indirect
## stub is actually needed, including for MOV stubs and shared reservations.
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux -mattr=+bti \
# RUN:   -aarch64-mark-bti-property --defsym=NOP=1 %t/skipped.s -o %t.nop.o
# RUN: ld.lld --emit-relocs -e caller %t.nop.o -o %t.nop
# RUN: llvm-bolt %t.nop -o %t.nop.bolt --turn-off-stubs --lite=0 \
# RUN:   --skip-funcs=callee --align-functions=4 --use-gnu-stack \
# RUN:   --custom-allocation-vma=0x40000000 --print-stub-stats \
# RUN:   | FileCheck %s --check-prefix=SHORT-STATS
# SHORT-STATS: JITLink stubs: slots=1 unused=0 short-adrp=1 long-mov=0 shared=2
# RUN: llvm-objdump -d --no-show-raw-insn --mattr=+bti \
# RUN:   --disassemble-symbols=callee %t.nop.bolt | FileCheck %s --check-prefix=PATCH
# PATCH-LABEL: <callee>:
# PATCH-NEXT: bti c
# PATCH-NEXT: mov x0, #0x2a
# PATCH-NEXT: ret
# RUN: llvm-bolt %t.nop -o %t.mov.bolt --turn-off-stubs --lite=0 \
# RUN:   --skip-funcs=callee --align-functions=4 --use-gnu-stack \
# RUN:   --custom-allocation-vma=0x1234500000 --print-stub-stats \
# RUN:   | FileCheck %s --check-prefix=MOV-STATS
# MOV-STATS: JITLink stubs: slots=1 unused=0 short-adrp=0 long-mov=1 shared=2
# RUN: llvm-objdump -d --no-show-raw-insn --mattr=+bti \
# RUN:   --disassemble-symbols=callee %t.mov.bolt | FileCheck %s --check-prefix=PATCH

## Existing landing pads on skipped functions are accepted without patching.
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux -mattr=+bti \
# RUN:   -aarch64-mark-bti-property --defsym=BTI=1 %t/skipped.s -o %t.bti.o
# RUN: ld.lld --emit-relocs -e caller %t.bti.o -o %t.bti
# RUN: llvm-bolt %t.bti -o %t.bti.bolt --turn-off-stubs --lite=0 \
# RUN:   --skip-funcs=callee --align-functions=4 --use-gnu-stack \
# RUN:   --custom-allocation-vma=0x40000000 --print-stub-stats \
# RUN:   | FileCheck %s --check-prefix=SHORT-STATS
# RUN: llvm-objdump -d --no-show-raw-insn --mattr=+bti \
# RUN:   --disassemble-symbols=callee %t.bti.bolt | FileCheck %s --check-prefix=PATCH

## Reject an unpatchable indirect target, but allow it to remain a direct call.
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux -mattr=+bti \
# RUN:   -aarch64-mark-bti-property %t/skipped.s -o %t.raw.o
# RUN: ld.lld --emit-relocs -e caller %t.raw.o -o %t.raw
# RUN: not llvm-bolt %t.raw -o %t.raw.bolt --turn-off-stubs --lite=0 \
# RUN:   --skip-funcs=callee --use-gnu-stack --custom-allocation-vma=0x40000000 \
# RUN:   2>&1 | FileCheck %s --check-prefix=ERROR
# ERROR: BOLT-ERROR: JITLink failed: Cannot materialize AArch64 branch stub for target 0x{{[0-9a-f]+}}: target has no compatible BTI landing pad or NOP to patch
# RUN: llvm-bolt %t.raw -o %t.near.bolt --turn-off-stubs --lite=0 \
# RUN:   --skip-funcs=callee --print-stub-stats \
# RUN:   | FileCheck %s --check-prefix=NEAR
# NEAR: JITLink stubs: slots=1 unused=1 short-adrp=0 long-mov=0 shared=0

## A pad at the symbol is insufficient when the actual target includes an
## addend. The interior instruction must not silently become an indirect entry.
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux -mattr=+bti \
# RUN:   -aarch64-mark-bti-property --defsym=BTI=1 --defsym=ADDEND=4 \
# RUN:   %t/skipped.s -o %t.addend.o
# RUN: ld.lld --emit-relocs -e caller %t.addend.o -o %t.addend
# RUN: not llvm-bolt %t.addend -o %t.addend.bolt --turn-off-stubs --lite=0 \
# RUN:   --skip-funcs=callee --use-gnu-stack --custom-allocation-vma=0x40000000 \
# RUN:   2>&1 | FileCheck %s --check-prefix=ERROR

## Synthetic fall-through guards also need protected destinations.
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux -mattr=+bti \
# RUN:   -aarch64-mark-bti-property %t/guard.s -o %t.guard.o
# RUN: ld.lld --emit-relocs -e first %t.guard.o -o %t.guard
# RUN: llvm-bolt %t.guard -o %t.guard.bolt --turn-off-stubs --lite=0 \
# RUN:   --pad-funcs-before=second:134217984 --print-stub-stats \
# RUN:   | FileCheck %s --check-prefix=GUARD-STATS
# GUARD-STATS: JITLink stubs: slots=1 unused=0 short-adrp=1 long-mov=0 shared=0
# RUN: llvm-objdump -d --no-show-raw-insn --mattr=+bti \
# RUN:   --disassemble-symbols=second %t.guard.bolt | FileCheck %s --check-prefix=GUARD
# GUARD-LABEL: <second>:
# GUARD-NEXT: bti c
# GUARD-NEXT: ret

## Hot-to-cold branches are handled before local relaxation; the inserted pad
## is part of the emitted fragment, not a late change to its size or addresses.
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux -mattr=+bti \
# RUN:   -aarch64-mark-bti-property %t/split.s -o %t.split.o
# RUN: ld.lld --emit-relocs -e foo %t.split.o -o %t.split
# RUN: link_fdata --no-lbr %t/split.s %t.split %t.fdata
# RUN: llvm-strip --strip-unneeded %t.split
# RUN: llvm-bolt %t.split -o %t.split.bolt --turn-off-stubs --lite=0 \
# RUN:   --data=%t.fdata --split-functions --align-functions=4 \
# RUN:   --pad-funcs-before=spacer:134217984 --print-stub-stats \
# RUN:   | FileCheck %s --check-prefix=GUARD-STATS
# RUN: llvm-objdump -d --no-show-raw-insn --mattr=+bti \
# RUN:   --disassemble-symbols=foo.cold.0 %t.split.bolt | FileCheck %s --check-prefix=COLD
# COLD-LABEL: <foo.cold.0>:
# COLD-NEXT: bti c
# COLD-NEXT: mov x0, #0x2a

#--- targets.s
.text
.globl caller
.type caller, %function
caller:
  bti c
  bl plain
  bl plain_alias
  bl jump_pad
  bl both_pad
  bl pac_a
  bl pac_b
  b plain
.size caller, .-caller

.globl plain, plain_alias
.type plain, %function
plain:
plain_alias:
  ret
.size plain, .-plain

.globl jump_pad
.type jump_pad, %function
jump_pad:
  bti j
  ret
.size jump_pad, .-jump_pad

.globl both_pad
.type both_pad, %function
both_pad:
  bti jc
  ret
.size both_pad, .-both_pad

.globl pac_a
.type pac_a, %function
pac_a:
  paciasp
  autiasp
  ret
.size pac_a, .-pac_a

.globl pac_b
.type pac_b, %function
pac_b:
  pacibsp
  autibsp
  ret
.size pac_b, .-pac_b

#--- skipped.s
.ifndef ADDEND
.set ADDEND, 0
.endif
.text
.globl caller
.type caller, %function
caller:
  bti c
  bl callee + ADDEND
  bl callee + ADDEND
  b callee + ADDEND
.size caller, .-caller

.globl callee
.type callee, %function
callee:
.ifdef NOP
  nop
.endif
.ifdef BTI
  bti c
.endif
  mov x0, #42
  ret
.size callee, .-callee

#--- guard.s
.text
.globl first
.type first, %function
first:
  blr x0
.size first, .-first
.globl second
.type second, %function
second:
  ret
.size second, .-second
.reloc 0, R_AARCH64_NONE

#--- split.s
.text
.globl foo
.type foo, %function
foo:
.hot:
# FDATA: 1 foo #.hot# 100
  bti c
  cbz x0, .cold
.hotret:
# FDATA: 1 foo #.hotret# 100
  ret
.cold:
  mov x0, #42
  .rept 8
  add x1, x1, #1
  .endr
  ret
.size foo, .-foo
.globl spacer
.type spacer, %function
spacer:
.spacerhot:
# FDATA: 1 spacer #.spacerhot# 100
  bti c
  ret
.size spacer, .-spacer
.reloc 0, R_AARCH64_NONE
