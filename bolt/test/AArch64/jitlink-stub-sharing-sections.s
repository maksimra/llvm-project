## Once final addresses are known, hot and cold fragments can share a stub.
## Calls through an alias must reach the same destination too.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %s -o %t.o
# RUN: ld.lld --emit-relocs -e caller %t.o -o %t.exe
# RUN: link_fdata --no-lbr %s %t.exe %t.fdata
# RUN: llvm-strip --strip-unneeded %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --data=%t.fdata --split-functions \
# RUN:   --funcs=caller --turn-off-stubs --lite=0 --align-functions=16 \
# RUN:   --custom-allocation-vma=0x1234500000 --use-gnu-stack \
# RUN:   --print-stub-stats --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=STATS

## Each section reserves a slot for callee. Only one is materialized. The
## third reservation is for the direct hot-to-cold branch and remains unused.
# STATS: BOLT-INFO: JITLink stubs: slots=3 unused=2 short-adrp=0 long-mov=1 shared=2
# RUN: llvm-objdump -d --no-show-raw-insn \
# RUN:   --disassemble-symbols=caller,caller.cold.0 %t.bolt | FileCheck %s
# CHECK: Disassembly of section .text:
# CHECK-LABEL: <caller>:
# CHECK-NEXT: bl 0x[[#%x,STUB:]]
# CHECK: Disassembly of section .text.cold:
# CHECK-LABEL: <caller.cold.0>:
# CHECK-NEXT: bl 0x[[#STUB]]
# CHECK-NEXT: bl 0x[[#STUB]]

.text
.globl caller
.type caller, %function
caller:
.hot:
# FDATA: 1 caller #.hot# 100
  bl callee
  cbz x0, .cold
.hotret:
# FDATA: 1 caller #.hotret# 100
  ret
.cold:
  bl callee_alias
  bl callee_alias
  .rept 8
  add x1, x1, #1
  .endr
  ret
.size caller, .-caller

.globl callee
.globl callee_alias
.type callee, %function
callee:
callee_alias:
  mov w0, #1
  ret
.size callee, .-callee
