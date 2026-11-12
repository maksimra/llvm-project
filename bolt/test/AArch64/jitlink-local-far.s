## A conditional branch's reserved B must enter the ordinary island pipeline
## before section bases are known. No compact-code-model/LongJmpPass is needed.

# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %s -o %t.o
# RUN: ld.lld --emit-relocs -e caller %t.o -o %t.exe
# RUN: link_fdata --no-lbr %s %t.exe %t.fdata
# RUN: llvm-strip --strip-unneeded %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --turn-off-stubs --lite=0 \
# RUN:   --data=%t.fdata --split-functions --align-functions=4 \
# RUN:   --pad-funcs-before=pad:134217984 --verify-branch26-range \
# RUN:   --print-stub-stats | FileCheck %s --check-prefix=STATS
# RUN: llvm-objdump -d --no-show-raw-insn \
# RUN:   --disassemble-symbols=caller,caller.cold.0,nonsimple %t.bolt \
# RUN:   | FileCheck %s

## CDSplit measures code before final emission. Deferred relaxation must also
## work here without the legacy compact-code-model requirement.
# RUN: llvm-bolt %t.exe -o %t.cdsplit --turn-off-stubs --lite=0 \
# RUN:   --data=%t.fdata --split-functions --split-strategy=cdsplit \
# RUN:   --verify-branch26-range

# STATS: JITLink stubs: slots=1 unused=0 short-adrp=1 long-mov=0
# CHECK-LABEL: <caller>:
# CHECK-NEXT: cbnz x0,
# CHECK-NEXT: b 0x[[#%x,STUB:]]
# CHECK-NEXT: ret

## Unknown indirect control flow makes this function non-simple. Its original
## instruction offsets must survive, even though it reserves a tail trampoline.
# CHECK-LABEL: <nonsimple>:
# CHECK-NEXT: [[#%x,NS:]]:
# CHECK-SAME: cbz x0, 0x[[#NS + 8]]
# CHECK-NEXT: br x1
# CHECK-NEXT: ret
# CHECK: {{0*}}[[#STUB]]: adrp x16,
# CHECK-NEXT: add x16, x16,
# CHECK-NEXT: br x16

# CHECK-LABEL: <caller.cold.0>:
# CHECK-NEXT: mov x0, #0x1
# CHECK-NEXT: add x0, x0, #0x1

.text
.globl caller
.type caller, %function
caller:
.hot:
# FDATA: 1 caller #.hot# 100
  cbz x0, .cold
.hotret:
# FDATA: 1 caller #.hotret# 100
  ret
.cold:
  mov x0, #1
  .rept 8
    add x0, x0, #1
  .endr
  ret
.size caller, .-caller

.globl nonsimple
.type nonsimple, %function
nonsimple:
.ns_entry:
# FDATA: 1 nonsimple #.ns_entry# 100
  cbz x0, .Lret
  br x1
.Lret:
  ret
.size nonsimple, .-nonsimple

.globl pad
.type pad, %function
pad:
.pad_entry:
# FDATA: 1 pad #.pad_entry# 100
  ret
.size pad, .-pad
.reloc 0, R_AARCH64_NONE
