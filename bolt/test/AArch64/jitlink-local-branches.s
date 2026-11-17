## Relax conditional branches only when emitting the final fragment bodies.
## Cascading expansions must account for both forward and backward limits;
## external tail calls must survive InstructionLowering and retain their target.

# REQUIRES: system-linux
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %t/local.s -o %t.o
# RUN: ld.lld --emit-relocs -e cascade %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --turn-off-stubs --lite=0 --keep-nops \
# RUN:   --align-functions=4 --print-after-lowering --print-only=cascade \
# RUN:   --verify-branch26-range | FileCheck %s --check-prefix=LATE
# RUN: llvm-objdump -d --no-show-raw-insn --disassemble-symbols=cascade,nonsimple \
# RUN:   %t.bolt | FileCheck %s --check-prefix=LOCAL

## No branch was reversed or split by LongJmpPass before lowering.
# LATE-LABEL: Binary Function "cascade" after inst-lowering {
# LATE: 00000000: tbz w0, #0x0,
# LATE: 00000004: tbz w1, #0x0,
# LATE: tbz w2, #0x0,
# LATE: b callee

# LOCAL-LABEL: <cascade>:
# LOCAL-NEXT: tbnz w0, #0x0, 0x[[#%x,FALL:]]
# LOCAL-NEXT: b 0x[[#%x,END:]]
# LOCAL-NEXT: {{0*}}[[#FALL]]: tbnz w1, #0x0, 0x[[#%x,PAD:]]
# LOCAL-NEXT: b 0x[[#%x,TC:]]
# LOCAL-NEXT: {{0*}}[[#PAD]]: nop
# LOCAL: {{0*}}[[#END]]: tbnz w2, #0x0, 0x[[#%x,TAIL:]]
# LOCAL-NEXT: b {{.*}} <cascade>
# LOCAL-NEXT: {{0*}}[[#TAIL]]: b {{.*}} <callee>
# LOCAL-NEXT: {{0*}}[[#TC]]: b {{.*}} <callee>

## A non-simple function keeps its internal instruction layout and shares an
## end-of-function trampoline between external conditional branches.
# LOCAL-LABEL: <nonsimple>:
# LOCAL-NEXT: tbz w0, #0x0, 0x[[#%x,TRAMPOLINE:]]
# LOCAL-NEXT: tbz w1, #0x0, 0x[[#TRAMPOLINE]]
# LOCAL-NEXT: br x2
# LOCAL: ret
# LOCAL-NEXT: {{0*}}[[#TRAMPOLINE]]: b {{.*}} <callee2>

# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %t/split.s -o %t.split.o
# RUN: ld.lld --emit-relocs -e foo %t.split.o -o %t.split
# RUN: llvm-bolt %t.split -o %t.split.bolt --turn-off-stubs --lite=0 \
# RUN:   --split-functions --split-strategy=all --align-functions=4 \
# RUN:   --print-after-lowering --print-only=foo --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=SPLIT-LATE
# RUN: llvm-objdump -d --no-show-raw-insn --disassemble-symbols=foo,foo.cold.0,foo.cold.1 \
# RUN:   %t.split.bolt > %t.dump
# RUN: llvm-dwarfdump --eh-frame %t.split.bolt >> %t.dump
# RUN: FileCheck %s --input-file=%t.dump --check-prefix=SPLIT

# SPLIT-LATE-LABEL: Binary Function "foo" after inst-lowering {
# SPLIT-LATE: tbz w0, #0x0,

## The expansion stays next to the original instruction, under the original
## CFA rule, even though FinalizeFunctions has already run.
# SPLIT: [[#%x,FOO:]] <foo>:
# SPLIT-NEXT: sub sp, sp, #0x10
# SPLIT-NEXT: tbnz w0, #0x0, 0x[[#%x,HOT:]]
# SPLIT-NEXT: b 0x[[#%x,COLD:]]
# SPLIT-NEXT: {{0*}}[[#HOT]]: b {{.*}} <foo.cold.0>
# SPLIT: <foo.cold.1>:
# SPLIT: {{0*}}[[#COLD]]: mov x0, #0x2a
# SPLIT-NEXT: b {{.*}} <foo.cold.0>
# SPLIT: FDE cie={{.*}} pc={{0*}}[[#FOO]]...{{0*}}[[#FOO + 16]]
# SPLIT: 0x[[#FOO + 4]]: CFA=WSP+16
# SPLIT-NOT: CFA=
# SPLIT: FDE cie=

# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux -mattr=+cmpbr \
# RUN:   %t/irreversible.s -o %t.irreversible.o
# RUN: ld.lld --emit-relocs -e irreversible %t.irreversible.o -o %t.irreversible
# RUN: llvm-bolt %t.irreversible -o %t.irreversible.bolt --turn-off-stubs \
# RUN:   --lite=0 --split-functions --split-strategy=all --fix-branches-with-liveness
# RUN: llvm-objdump -d --no-show-raw-insn --mattr=+cmpbr \
# RUN:   --disassemble-symbols=irreversible %t.irreversible.bolt \
# RUN:   | FileCheck %s --check-prefix=IRREVERSIBLE

## CMPBR at the immediate boundary cannot be inverted without clobbering live
## flags. The expansion must keep both its predicate and the flags unchanged.
# IRREVERSIBLE-LABEL: <irreversible>:
# IRREVERSIBLE-NEXT: cmp x1, #0x0
# IRREVERSIBLE-NEXT: cbgt x0, #0x3f, 0x[[#%x,TAKEN:]]
# IRREVERSIBLE-NEXT: b 0x[[#%x,END:]]
# IRREVERSIBLE-NEXT: {{0*}}[[#TAKEN]]: b {{.*}} <irreversible.cold.1>
# IRREVERSIBLE-NEXT: {{0*}}[[#END]]: b {{.*}} <irreversible.cold.0>

# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %t/unreachable.s -o %t.bad.o
# RUN: ld.lld --emit-relocs -e bad %t.bad.o -o %t.bad
# RUN: not llvm-bolt %t.bad -o %t.bad.bolt --turn-off-stubs --lite=0 --keep-nops \
# RUN:   2>&1 | FileCheck %s --check-prefix=ERROR
# ERROR: error: cannot relax branch in function bad: end-of-function trampoline is out of reach
# ERROR: BOLT-ERROR: Emission failed.

#--- local.s
.text
.globl callee
.type callee, %function
callee:
  ret
.size callee, .-callee

.globl cascade
.type cascade, %function
cascade:
.Lentry:
  tbz w0, #0, .Lend
  tbz w1, #0, callee
  .rept 8189
  nop
  .endr
.Lend:
  tbz w2, #0, .Lentry
  b callee
.size cascade, .-cascade

.globl nonsimple
.type nonsimple, %function
nonsimple:
  tbz w0, #0, callee2
  tbz w1, #0, callee2
  br x2
  .rept 8
  nop
  .endr
  ret
.size nonsimple, .-nonsimple

.globl callee2
.type callee2, %function
callee2:
  ret
.size callee2, .-callee2

#--- split.s
.text
.globl foo
.type foo, %function
foo:
  .cfi_startproc
  sub sp, sp, #16
  .cfi_def_cfa_offset 16
  tbz w0, #0, .Lcold
.Lhot:
  add sp, sp, #16
  .cfi_def_cfa_offset 0
  ret
.Lcold:
  .cfi_def_cfa_offset 16
  mov x0, #42
  b .Lhot
  .cfi_endproc
.size foo, .-foo
.reloc 0, R_AARCH64_NONE

#--- irreversible.s
.text
.globl irreversible
.type irreversible, %function
irreversible:
  cmp x1, #0
  cbgt x0, #63, .Ltaken
  mov x0, x1
  ret
.Ltaken:
  csel x0, x1, x2, eq
  ret
.size irreversible, .-irreversible
.reloc 0, R_AARCH64_NONE

#--- unreachable.s
.text
.globl callee
.type callee, %function
callee:
  ret
.size callee, .-callee
.globl bad
.type bad, %function
bad:
  tbz w0, #0, callee
  br x2
  .rept 8192
  nop
  .endr
  ret
.size bad, .-bad
