## An unused pessimistic PIC reservation must remain NOP-filled and must not
## create Page21 fixups. Also document the one-sided tradeoff: a required
## post-batch group may be outside Page21 reach even though the removed
## pre-batch group would have been reachable.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple=aarch64-linux %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -q -pie --entry=source \
# RUN:   --section-start=.oldtext=0x10000 --section-start=.text=0x20000
# RUN: llvm-bolt %t.exe -o %t.bolt --lite=0 --skip-funcs=far_target \
# RUN:   --jitlink-branch26-relaxation --verify-branch26-range 2>&1 \
# RUN:   | FileCheck %s --check-prefix=UNUSED
# RUN: llvm-objdump -d --show-all-symbols %t.bolt \
# RUN:   | FileCheck %s --check-prefix=NO-THUNK

# RUN: llvm-mc -filetype=obj -triple=aarch64-linux -defsym=FAR=1 %s \
# RUN:   -o %t.far.o
# RUN: ld.lld %t.far.o -o %t.far.exe -q -pie --entry=source \
# RUN:   --section-start=.oldtext=0x10000 --section-start=.text=0x20000 \
# RUN:   --section-start=.high=0xfc000000
# RUN: not llvm-bolt %t.far.exe -o %t.far.bolt --lite=0 \
# RUN:   --skip-funcs=far_target \
# RUN:   --pad-funcs=source:125829120 \
# RUN:   --jitlink-branch26-relaxation 2>&1 \
# RUN:   | FileCheck %s --check-prefix=ONE-SIDED

# UNUSED: BOLT-INFO: JITLink Branch26 relaxation: batches=1, groups=1, reserved=1, used=0, direct=1, relaxed=0
# UNUSED: BOLT-INFO: AArch64 Branch26PCRel edges:
# UNUSED-SAME: out-of-range=0
# UNUSED-NOT: out of range of Page21

# NO-THUNK-NOT: adrp x16
# NO-THUNK-NOT: br x16

# ONE-SIDED: BOLT-ERROR: experimental JITLink Branch26 relaxation cannot materialize
# ONE-SIDED-SAME: PIC target far_target + 0 from source[fragment 0]+0x0:
# ONE-SIDED-SAME: the post-batch thunk group ADRP page displacement is
# ONE-SIDED-SAME: {{-?[0-9]+}}; the supported Page21 range is +/-4 GiB
# ONE-SIDED-NOT: out of range of Page21

  .section .text,"ax",@progbits
  .globl source
  .type source, %function
source:
  bl far_target
  ret
  .size source, .-source

  .section .oldtext,"ax",@progbits
  .globl far_target
  .type far_target, %function
far_target:
  ret
  .size far_target, .-far_target

.ifndef FAR
  .set FAR, 0
.endif

## Raise BOLT's allocation address without creating a large input file.
.if FAR
  .section .high,"a",@progbits
  .byte 0
.endif

  .reloc 0, R_AARCH64_NONE
