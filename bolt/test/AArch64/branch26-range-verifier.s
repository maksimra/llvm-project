## Check the opt-in JITLink Branch26 range verifier. ELF CALL26 and JUMP26
## relocations both become Branch26PCRel edges; the original opcode lets the
## diagnostic count them separately.

# REQUIRES: system-linux

# RUN: %clang %cflags -Wl,-q %s -o %t
# RUN: link_fdata --no-lbr %s %t %t.fdata
# RUN: llvm-bolt %t -o %t.off.bolt --compact-code-model --lite=0 \
# RUN:   --data %t.fdata 2>&1 | FileCheck %s --check-prefix=OFF
# RUN: llvm-readelf -S %t.off.bolt | FileCheck %s --check-prefix=BASE-LAYOUT
# RUN: llvm-bolt %t -o %t.bolt --compact-code-model --lite=0 \
# RUN:   --data %t.fdata --verify-branch26-range 2>&1 \
# RUN:   | FileCheck %s --check-prefix=IN-RANGE
# RUN: llvm-objcopy --dump-section=.text=%t.off.text %t.off.bolt
# RUN: llvm-objcopy --dump-section=.text=%t.verify.text %t.bolt
# RUN: cmp %t.off.text %t.verify.text
# RUN: llvm-nm -nS --format=posix %t.off.bolt \
# RUN:   | awk '$1=="main" || $1=="callee" || $1=="tail" || \
# RUN:          $1=="splitme" || $1=="large_function"' > %t.off.symbols
# RUN: llvm-nm -nS --format=posix %t.bolt \
# RUN:   | awk '$1=="main" || $1=="callee" || $1=="tail" || \
# RUN:          $1=="splitme" || $1=="large_function"' > %t.verify.symbols
# RUN: diff %t.off.symbols %t.verify.symbols
# RUN: %clang %cflags -Wl,-q -Wa,-defsym,RESERVE_SPACE=1 %s -o %t.large
# RUN: link_fdata --no-lbr %s %t.large %t.large.fdata
# RUN: not llvm-bolt %t.large -o %t.large.bolt --data %t.large.fdata \
# RUN:   --split-functions --compact-code-model --verify-branch26-range 2>&1 \
# RUN:   | FileCheck %s --check-prefix=OUT-OF-RANGE

  .text
.ifndef RESERVE_SPACE
  .set RESERVE_SPACE, 0
.endif

  .globl main
  .type main, %function
main:
.entry_main:
# FDATA: 1 main #.entry_main# 1
  bl callee
  bl fake_plt
  b fake_plt

  .globl callee
  .type callee, %function
callee:
  ret

  .globl tail
  .type tail, %function
tail:
  mov w0, wzr
  ret

  .globl splitme
  .type splitme, %function
splitme:
.entry_splitme:
# FDATA: 1 splitme #.entry_splitme# 10
  cbz x0, .Lsplitme_cold
  mov x0, #1
.Lsplitme_cold:
  ret

  .globl large_function
  .type large_function, %function
large_function:
  ret

.if RESERVE_SPACE
  .space 0x8000000
.endif

## Force relocation mode.
.reloc 0, R_AARCH64_NONE

## Keep a real Branch26 edge to an executable PLT section so the verifier's
## target classification is covered without relying on a system libc.
  .section .plt,"ax",@progbits
  .globl fake_plt
  .type fake_plt, %function
fake_plt:
  ret
  .size fake_plt, .-fake_plt

# OFF-NOT: AArch64 Branch26PCRel edges:

# IN-RANGE: BOLT-INFO: AArch64 Branch26PCRel edges:
# IN-RANGE-SAME: total=3, out-of-range=0,
# IN-RANGE-SAME: CALL26=2, JUMP26=1,
# IN-RANGE-SAME: external=0, old-text=0, PLT=2,
# IN-RANGE-SAME: BOLT-code=1

# BASE-LAYOUT: .text             PROGBITS        0000000000400000 {{[0-9a-f]+}} 000018
# BASE-LAYOUT: .text.cold        PROGBITS        0000000000400040 {{[0-9a-f]+}} 000010
# OUT-OF-RANGE: BOLT-WARNING: out-of-range Branch26PCRel CALL26 edge
# OUT-OF-RANGE: BOLT-INFO: AArch64 Branch26PCRel edges: total={{[1-9][0-9]*}}, out-of-range={{[1-9][0-9]*}}
# OUT-OF-RANGE: BOLT-ERROR: JITLink failed: {{.*}}out of range of Branch26PCRel
