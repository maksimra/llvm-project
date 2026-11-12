## Reserving long conditional forms can also grow a local ADR displacement.
## The original function is smaller than 1 MiB. Four branch reservations move
## its final ADR beyond the negative limit; its original size is not a proof
## that the ADR can remain short.

# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %s -o %t.o
# RUN: ld.lld --emit-relocs -e _start %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --turn-off-stubs --lite=0 \
# RUN:   --verify-branch26-range
# RUN: llvm-objdump -d --no-show-raw-insn --disassemble-symbols=adr_site,small \
# RUN:   %t.bolt | FileCheck %s

# CHECK-LABEL: <adr_site>:
# CHECK-NEXT: adrp x1,
# CHECK-NEXT: add x1, x1,
# CHECK-NEXT: ret

## Even a small movable function uses ADRP; no size estimate selects ADR.
# CHECK-LABEL: <small>:
# CHECK-NEXT: adrp x1,
# CHECK-NEXT: add x1, x1,
# CHECK-NEXT: ret

.text
.globl _start
.type _start, %function
_start:
  tbz w0, #0, .Lnear0
  add x0, x0, #1
.Lnear0:
  tbz w0, #1, .Lnear1
  add x0, x0, #1
.Lnear1:
  tbz w0, #2, .Lnear2
  add x0, x0, #1
.Lnear2:
  tbz w0, #3, .Lnear3
  add x0, x0, #1
.Lnear3:
  .rept 262133
    add x2, x2, #1
  .endr
adr_site:
  adr x1, _start
  ret
.size _start, .-_start
.reloc 0, R_AARCH64_NONE

.globl small
.type small, %function
small:
  adr x1, small
  ret
.size small, .-small
