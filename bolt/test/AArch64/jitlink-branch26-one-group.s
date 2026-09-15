## A batch uses one post-batch group. The earliest source must reach the last
## deduplicated slot near the positive Branch26 limit, even when the original
## target is behind the source.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple=aarch64-linux %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -q --entry=source --image-base=0 \
# RUN:   --section-start=.oldtext=0x10000 --section-start=.text=0x20000 \
# RUN:   --section-start=.high=0x10000000
# RUN: llvm-bolt %t.exe -o %t.bolt --lite=0 --skip-funcs=target \
# RUN:   --pad-funcs=source:134217600 --jitlink-branch26-relaxation \
# RUN:   --verify-branch26-range 2>&1 | FileCheck %s --check-prefix=STATS
# RUN: llvm-objdump -d --show-all-symbols %t.bolt \
# RUN:   | FileCheck %s --check-prefix=DISASM

# STATS: BOLT-INFO: JITLink Branch26 relaxation: batches=1, groups=1, reserved=2, used=2, direct=0, relaxed=3, semantic-blocks=1, group-bytes=40
# STATS: BOLT-INFO: AArch64 Branch26PCRel edges: total=3, out-of-range=0, CALL26=2, JUMP26=1

## The two references to target share the first slot. target+4 uses the second
## and last slot, which is reached by the source at address +4.
# DISASM-LABEL: <source>:
# DISASM-NEXT: {{[0-9a-f]+}}: {{[0-9a-f]+}}      bl [[FIRST:0x[0-9a-f]+]] <source+0x7ffff8c>
# DISASM-NEXT: {{[0-9a-f]+}}: {{[0-9a-f]+}}      bl [[LAST:0x[0-9a-f]+]] <source+0x7ffffa0>
# DISASM-NEXT: {{[0-9a-f]+}}: {{[0-9a-f]+}}      b [[FIRST]] <source+0x7ffff8c>
# DISASM: {{[0-9a-f]+}}: d2e00010      movz x16
# DISASM: {{[0-9a-f]+}}: d2e00010      movz x16

  .section .text,"ax",@progbits
  .globl source
  .type source, %function
source:
  bl target
  bl target+4
  b target
  ret
  .size source, .-source

  .section .oldtext,"ax",@progbits
  .globl target
  .type target, %function
target:
  ret
  ret
  .size target, .-target

## Raise BOLT's allocation address without creating a large input file.
  .section .high,"a",@progbits
  .byte 0

  .reloc 0, R_AARCH64_NONE
