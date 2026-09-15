## Page21/PageOffset12 references whose section-symbol addend crosses a
## semantic split must keep the same effective target.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple=aarch64-linux %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -q --entry=_start
# RUN: llvm-bolt %t.exe -o %t.base --align-functions=64 \
# RUN:   --align-functions-max-bytes=8
# RUN: rm -rf %t.tmpdir && mkdir %t.tmpdir
# RUN: env TMPDIR=%t.tmpdir llvm-bolt %t.exe -o %t.semantic \
# RUN:   --align-functions=64 --align-functions-max-bytes=8 \
# RUN:   --jitlink-branch26-relaxation --keep-tmp 2>&1 \
# RUN:   | FileCheck %s --check-prefix=STATS
# RUN: llvm-readelf -r %t.tmpdir/output-*.o \
# RUN:   | FileCheck %s --check-prefix=RELOCS
# RUN: llvm-objcopy --dump-section=.text=%t.base.text %t.base
# RUN: llvm-objcopy --dump-section=.text=%t.semantic.text %t.semantic
# RUN: cmp %t.base.text %t.semantic.text

# STATS: BOLT-INFO: JITLink Branch26 relaxation: batches=1, groups=0, reserved=0, used=0, direct=0, relaxed=0, semantic-blocks=2

## The temporary object proves that these enter JITLink as section-symbol plus
## addend edges. The addend is f2's offset and crosses the split at f2.
# RELOCS: R_AARCH64_ADR_PREL_PG_HI21 {{.*}} .text + c
# RELOCS: R_AARCH64_ADD_ABS_LO12_NC {{.*}} .text + c

  .text
  .globl _start
  .type _start, %function
_start:
  .inst 0x90000000 // adrp x0, 0
  .reloc _start, R_AARCH64_ADR_PREL_PG_HI21, .text + 12
  .inst 0x91000000 // add x0, x0, #0
  .reloc _start + 4, R_AARCH64_ADD_ABS_LO12_NC, .text + 12
  ret
  .size _start, .-_start

  .globl f2
  .type f2, %function
f2:
  ret
  .size f2, .-f2

## Force relocation mode.
.reloc 0, R_AARCH64_NONE
