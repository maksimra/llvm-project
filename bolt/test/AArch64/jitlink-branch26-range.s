## Verify final Branch26 ranges before JITLink applies fixups, including in
## runtime objects, where BOLT does not perform branch relaxation.
# REQUIRES: system-linux
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %t/main.s -o %t.o
# RUN: ld.lld --emit-relocs -e _start %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.near --lite=0 --verify-branch26-range
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %t/runtime.s -o %t.rt.o
# RUN: not llvm-bolt %t.exe -o %t.far --lite=0 --hugify \
# RUN:   --runtime-hugify-lib=%t.rt.o --verify-branch26-range 2>&1 | FileCheck %s
# CHECK: BOLT-ERROR: JITLink failed: AArch64 Branch26 target out of range before fixups: branch at 0x{{[0-9a-f]+}} in .text targets 0x1234567890

#--- main.s
.text
.globl _start
.type _start, %function
_start:
  ret
.size _start, .-_start

#--- runtime.s
.text
.globl __bolt_hugify_self
.type __bolt_hugify_self, %function
__bolt_hugify_self:
  .inst 0x14000000
  .reloc .-4, R_AARCH64_JUMP26, far_target
.size __bolt_hugify_self, .-__bolt_hugify_self
.globl far_target
.set far_target, 0x1234567890
