## Incoming section-symbol-plus-addend relocations must retain their effective
## targets when the emitted .text block is split at semantic markers.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple=aarch64-linux %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -q --entry=_start
# RUN: llvm-bolt %t.exe -o %t.base --align-functions=64 \
# RUN:   --align-functions-max-bytes=8
# RUN: rm -rf %t.tmpdir && mkdir %t.tmpdir
# RUN: env TMPDIR=%t.tmpdir llvm-bolt %t.exe -o %t.semantic --align-functions=64 \
# RUN:   --align-functions-max-bytes=8 --jitlink-branch26-relaxation \
# RUN:   --verify-branch26-range --keep-tmp 2>&1 \
# RUN:   | FileCheck %s --check-prefix=STATS
# RUN: llvm-readelf -r %t.tmpdir/output-*.o \
# RUN:   | FileCheck %s --check-prefix=RELOCS
# RUN: llvm-objcopy --dump-section=.data=%t.base.data %t.base
# RUN: llvm-objcopy --dump-section=.data=%t.semantic.data %t.semantic
# RUN: cmp %t.base.data %t.semantic.data
# RUN: llvm-readelf -x .data %t.semantic | FileCheck %s --check-prefix=DATA

# STATS: BOLT-INFO: JITLink Branch26 relaxation: batches=1, groups=0, reserved=0, used=0, direct=3, relaxed=0, semantic-blocks=4
# STATS: BOLT-INFO: AArch64 Branch26PCRel edges: total=3, out-of-range=0

## BOLT canonicalizes the source spellings f2+4 and f2-4 to exact emitted
## symbols before BinaryEmitter. The temporary ELF therefore contains three
## distinct collision-proof aliases with zero addends and preserves all three
## effective targets without relying on assembler relaxation.
# RELOCS: R_AARCH64_CALL26 {{.*}}branch26_target_0_2_f2_{{[0-9]+}} + 0
# RELOCS: R_AARCH64_CALL26 {{.*}}branch26_target_1_{{[0-9]+}}___ENTRY_f2@{{.*}} + 0
# RELOCS: R_AARCH64_CALL26 {{.*}}branch26_target_2_2_f1_{{[0-9]+}} + 0

## With the test's deterministic 0x600000 output base, these are respectively
## f2 (one split), f3 (multiple splits), f2-4 (the preceding partition), and
## the zero-sized address exactly at the emitted parent block end.
# DATA: 0x{{0*}}220178 14006000 00000000 18006000 00000000
# DATA-NEXT: 0x{{0*}}220188 10006000 00000000 1c006000 00000000

  .text
  .globl _start
  .type _start, %function
_start:
  bl f2
  bl f2+4
  bl f2-4
  ret
  .size _start, .-_start

  .globl f1
  .type f1, %function
f1:
  ret
  .size f1, .-f1

  .globl f2
  .type f2, %function
f2:
  nop
  ret
  .size f2, .-f2

  .globl f3
  .type f3, %function
f3:
  ret
  .size f3, .-f3

  .data
  .globl refs
refs:
  .quad .text + (f2 - .text)
  .quad .text + (f3 - .text)
  .quad f2 - 4
  .quad f3 + 4
  .size refs, .-refs

  .reloc 0, R_AARCH64_NONE
