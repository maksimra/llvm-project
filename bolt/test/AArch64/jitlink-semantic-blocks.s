## Check the semantic function-block representation used by the experimental
## JITLink Branch26 path. The final output layout must remain identical to the
## normal BOLT layout when no range extension is needed.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple=aarch64-linux %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -q --entry=_start
# RUN: llvm-bolt %t.exe -o %t.base --align-functions=64 \
# RUN:   --align-functions-max-bytes=1 --pad-funcs-before=callee:64 \
# RUN:   --pad-funcs=callee:64,tail:128
# RUN: llvm-bolt %t.exe -o %t.semantic --align-functions=64 \
# RUN:   --align-functions-max-bytes=1 --jitlink-branch26-relaxation \
# RUN:   --verify-branch26-range --pad-funcs-before=callee:64 \
# RUN:   --pad-funcs=callee:64,tail:128 2>&1 \
# RUN:   | FileCheck %s --check-prefix=SEMANTIC

## Function sizes must be identical. All targets are in the same batch, so no
## thunk groups are needed and no thunk can become part of a symbol range.
# RUN: llvm-readelf -sW %t.base | grep ' FUNC ' \
# RUN:   | awk '{print $3, $8}' | sort > %t.base.funcs
# RUN: llvm-readelf -sW %t.semantic | grep ' FUNC ' \
# RUN:   | awk '{print $3, $8}' | sort > %t.semantic.funcs
# RUN: diff %t.base.funcs %t.semantic.funcs
# RUN: echo BASE > %t.relative
# RUN: llvm-nm -n --format=posix %t.base >> %t.relative
# RUN: echo SEMANTIC >> %t.relative
# RUN: llvm-nm -n --format=posix %t.semantic >> %t.relative
# RUN: FileCheck %s --input-file=%t.relative --check-prefix=RELATIVE
# RUN: llvm-nm -nS --format=posix %t.semantic > %t.fdes
# RUN: llvm-dwarfdump --eh-frame %t.semantic >> %t.fdes
# RUN: FileCheck %s --input-file=%t.fdes --check-prefix=FDES
# RUN: llvm-readelf -sW %t.semantic | FileCheck %s --check-prefix=ALIGN
# RUN: llvm-objdump -d -j .text %t.semantic | FileCheck %s \
# RUN:   --check-prefix=UNUSED

## Temporary section identities are gone before allocation.
# RUN: llvm-readelf -SW %t.semantic | FileCheck %s --check-prefix=SECTIONS
# RUN: llvm-bolt %t.exe -o %t.limit --align-functions=64 \
# RUN:   --align-functions-max-bytes=1 --jitlink-branch26-relaxation \
# RUN:   --verify-branch26-range --pad-funcs-before=callee:132100000 2>&1 \
# RUN:   | FileCheck %s --check-prefix=LIMIT

# SEMANTIC: BOLT-INFO: JITLink Branch26 relaxation: batches=1, groups=0, reserved=0, used=0, direct=3, relaxed=0, semantic-blocks=3, group-bytes=0
# SEMANTIC: BOLT-INFO: AArch64 Branch26PCRel edges: total=3, out-of-range=0, CALL26=1, JUMP26=2

## All three edges stay in one batch near the architectural bounds, including
## the planner's worst-case 2 MiB section-alignment gap: _start reaches
## callee/tail forwards and tail reaches _start backwards.
# LIMIT: BOLT-INFO: JITLink Branch26 relaxation: batches=1, groups=0, reserved=0, used=0, direct=3, relaxed=0
# LIMIT-SAME: semantic-blocks=3
# LIMIT: BOLT-INFO: AArch64 Branch26PCRel edges: total=3, out-of-range=0, CALL26=1, JUMP26=2

## Splitting and repacking changes a batch by one translation only. This
## checks the two independent relative distances across leading function
## padding, rejected MaxBytesToEmit alignment, trailing padding, and three
## semantic blocks.
# RELATIVE: BASE
# RELATIVE: _start T [[#%x,BASE_START:]]
# RELATIVE: callee T [[#%x,BASE_CALLEE:]]
# RELATIVE: tail T [[#%x,BASE_TAIL:]]
# RELATIVE: SEMANTIC
# RELATIVE: _start T [[#%x,NEW_START:]]
# RELATIVE: callee T [[#%x,NEW_START+BASE_CALLEE-BASE_START]]
# RELATIVE: tail T [[#%x,NEW_START+BASE_TAIL-BASE_START]]

## The FDE ranges include requested trailing function padding, but never
## linker-generated storage.
# FDES: _start T [[#%x,START:]] 8
# FDES: callee T [[#%x,CALLEE:]] 44
# FDES: tail T [[#%x,TAIL:]] 84
# FDES-DAG: pc={{0*}}[[#%x,START]]...{{0*}}[[#%x,START+8]]
# FDES-DAG: pc={{0*}}[[#%x,CALLEE]]...{{0*}}[[#%x,CALLEE+0x44]]
# FDES-DAG: pc={{0*}}[[#%x,TAIL]]...{{0*}}[[#%x,TAIL+0x84]]

## The preferred 64-byte alignment for callee is rejected by MaxBytesToEmit=1;
## its requested 64 bytes of leading padding start immediately after _start.
# ALIGN: [[#%.16x,START:]]     8 FUNC {{.*}} _start
# ALIGN-NEXT: [[#%.16x,START+0x48]]    68 FUNC {{.*}} callee

# SECTIONS: .text             PROGBITS
# SECTIONS-NOT: .bolt.jitlink

## No thunk instructions are emitted because both edges are stable within the
## same batch.
# UNUSED-NOT: adrp
# UNUSED-NOT: br x16
# UNUSED-NOT: movz x16
# UNUSED: <_start>:

  .text
  .globl _start
  .type _start, %function
_start:
  .cfi_startproc
  bl callee
  b tail
  .cfi_endproc
  .size _start, .-_start

  .globl callee
  .type callee, %function
callee:
  .cfi_startproc
  ret
  .cfi_endproc
  .size callee, .-callee

  .globl tail
  .type tail, %function
tail:
  .cfi_startproc
  b _start
  .cfi_endproc
  .size tail, .-tail

## Force relocation mode.
.reloc 0, R_AARCH64_NONE
