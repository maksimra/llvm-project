## Local relaxation must use emitted offsets, including basic-block alignment.
## Without padding the TBZ target is at 32760. Aligning the final block to 16
## bytes moves it to 32768, outside TBZ's positive range. The old LongJmpPass
## accepted the unaligned estimate and MC subsequently failed to emit it.

# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %s -o %t.o
# RUN: ld.lld --emit-relocs -e _start %t.o -o %t.exe
# RUN: link_fdata --no-lbr %s %t.exe %t.fdata
# RUN: llvm-bolt %t.exe -o %t.bolt --turn-off-stubs --lite=0 \
# RUN:   --data=%t.fdata --align-blocks --align-blocks-threshold=0 \
# RUN:   --block-alignment=16 --verify-branch26-range --print-stub-stats \
# RUN:   | FileCheck %s --check-prefix=LOG
# RUN: llvm-objdump -d --no-show-raw-insn --section=.text %t.bolt | FileCheck %s
# RUN: llvm-dwarfdump --eh-frame %t.bolt | FileCheck %s --check-prefix=FDE
# RUN: llvm-readelf -S %t.bolt | FileCheck %s --check-prefix=SECTIONS

# LOG-NOT: relaxing branches for compact code model
# LOG-NOT: Starting stub-insertion pass
# LOG: JITLink stubs: slots=0 unused=0 short-adrp=0 long-mov=0
# SECTIONS-NOT: .bolt.branch_info
# SECTIONS-NOT: .bolt.fragment.

# CHECK-LABEL: <_start>:
# CHECK-NEXT: [[#%x,SOURCE:]]:
# CHECK-SAME: tbnz w0, #0x0, 0x[[#SOURCE + 8]]
# CHECK-NEXT: b 0x[[#SOURCE + 32768]]
# CHECK: [[#SOURCE + 32768]]: add x0, x0, #0x1

## A near target restores the original condition and clears the long-leg edge.
# CHECK-LABEL: <near>:
# CHECK-NEXT: [[#%x,NEAR:]]:
# CHECK-SAME: tbz w0, #0x0, 0x[[#NEAR + 12]]
# CHECK-NEXT: nop
# CHECK-NEXT: add x0, x0, #0x1
# CHECK: [[#NEAR + 12]]: ret

## AL/NV are always taken, so their long forms must not invert the condition.
# CHECK-LABEL: <always>:
# CHECK-NEXT: [[#%x,ALWAYS:]]:
# CHECK-SAME: b.al 0x[[#ALWAYS + 16]]
# CHECK-NEXT: nop
# CHECK-NEXT: nop
# CHECK-NEXT: add x0, x0, #0x1
# CHECK: [[#ALWAYS + 16]]: ret

## The FDE covers the emitted reservation and alignment bytes.
# FDE: FDE cie={{.*}} pc=[[#%x,START:]]
# FDE-SAME: ...{{0*}}[[#START + 32788]]

.text
.globl _start
.type _start, %function
_start:
.entry:
# FDATA: 1 _start #.entry# 100
  .cfi_startproc
  tbz w0, #0, .target
.body:
# FDATA: 1 _start #.body# 1
  .rept 8189
    add x1, x1, #1
  .endr
.target:
# FDATA: 1 _start #.target# 100
  .rept 4
    add x0, x0, #1
  .endr
  ret
  .cfi_endproc
.size _start, .-_start

.globl near
.type near, %function
near:
.near_entry:
# FDATA: 1 near #.near_entry# 10
  tbz w0, #0, .near_ret
  add x0, x0, #1
.near_ret:
  ret
.size near, .-near

.globl always
.type always, %function
always:
.always_entry:
# FDATA: 1 always #.always_entry# 10
  b.al .always_ret
  add x0, x0, #1
.always_ret:
  ret
.size always, .-always

.reloc 0, R_AARCH64_NONE
