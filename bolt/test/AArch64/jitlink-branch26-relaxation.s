## Exercise exact post-allocation Branch26 relaxation through a preallocated
## post-batch group. The hidden padding options create large semantic blocks
## without making the input linker insert veneers.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple=aarch64-linux %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -q --entry=_start
# RUN: llvm-bolt %t.exe -o %t.bolt --lite=0 \
# RUN:   --jitlink-branch26-relaxation --verify-branch26-range \
# RUN:   --pad-funcs-before=source_right:134221824 2>&1 \
# RUN:   | FileCheck %s --check-prefix=RELAX
# RUN: llvm-objdump -d --show-all-symbols %t.bolt \
# RUN:   | FileCheck %s --check-prefix=DISASM
# RUN: llvm-bolt %t.exe -o %t.disabled --lite=0 2>&1 \
# RUN:   | FileCheck %s --check-prefix=DISABLED

## A source more than 128 MiB from both boundaries must fail before normal
## fixups rather than allocating a late group.
# RUN: not llvm-bolt %t.exe -o %t.pathological --lite=0 \
# RUN:   --jitlink-branch26-relaxation \
# RUN:   --pad-funcs-before=source_right:134221824 \
# RUN:   --pad-funcs=source_right:134221824 2>&1 \
# RUN:   | FileCheck %s --check-prefix=PATHOLOGICAL

# RELAX: BOLT-INFO: JITLink Branch26 relaxation: batches=1, groups=1, reserved=1, used=1, direct=5, relaxed=1, semantic-blocks=5, group-bytes=20
# RELAX: BOLT-INFO: AArch64 Branch26PCRel edges: total=6, out-of-range=0, CALL26=5, JUMP26=1

## The backwards source_right edge branches forward to the single post-batch
## group. All target_right calls and the jump remain direct.
# DISASM-LABEL: <source_right>:
# DISASM-NEXT: {{[0-9a-f]+}}: {{[0-9a-f]+}}      bl [[GROUP:0x[0-9a-f]+]] <_start+0x8>
# DISASM-NEXT: {{[0-9a-f]+}}: d65f03c0      ret
# DISASM-LABEL: <source_left>:
# DISASM-NEXT: {{[0-9a-f]+}}: {{[0-9a-f]+}}      bl [[TARGET:0x[0-9a-f]+]] <target_right>
# DISASM-NEXT: {{[0-9a-f]+}}: {{[0-9a-f]+}}      bl [[TARGET]] <target_right>
# DISASM-NEXT: {{[0-9a-f]+}}: {{[0-9a-f]+}}      bl {{.*}} <target_right+0x4>
# DISASM-NEXT: {{[0-9a-f]+}}: {{[0-9a-f]+}}      b [[TARGET]] <target_right>
# DISASM-LABEL: <_start>:
# DISASM-NEXT: {{[0-9a-f]+}}: {{[0-9a-f]+}}      bl {{.*}} <target_right>
# DISASM-NEXT: {{[0-9a-f]+}}: d65f03c0      ret
# DISASM-NEXT: {{[0-9a-f]+}}: d2e00010      movz x16, #0x0, lsl #48
# DISASM: {{[0-9a-f]+}}: d61f0200      br x16
# DISASM-NOT: __AArch64{{.*}}Thunk

# PATHOLOGICAL: BOLT-ERROR: experimental JITLink Branch26 relaxation cannot place
# PATHOLOGICAL-SAME: a post-batch thunk group for semantic block source_right[fragment 0]:
# PATHOLOGICAL-SAME: batch source-to-end span 134221832, group size 20,
# PATHOLOGICAL-SAME: Branch26 positive limit 134217724

# DISABLED: BOLT-INFO: Starting stub-insertion pass
# DISABLED-NOT: JITLink Branch26

  .text
  .globl target_left
  .type target_left, %function
target_left:
  ret
  ret
  .size target_left, .-target_left

  .globl source_right
  .type source_right, %function
source_right:
  bl target_left
  ret
  .size source_right, .-source_right

  .globl source_left
  .type source_left, %function
source_left:
  bl target_right
  bl target_right
  bl target_right+4
  b target_right
  .size source_left, .-source_left

  .globl target_right
  .type target_right, %function
target_right:
  ret
  ret
  .size target_right, .-target_right

  .globl _start
  .type _start, %function
_start:
  bl target_right
  ret
  .size _start, .-_start

  .reloc 0, R_AARCH64_NONE
