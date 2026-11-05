## Private function sections must not turn bounded entry alignment into
## unconditional block alignment. With no islands, placement matches classic.

# REQUIRES: system-linux
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %t/functions.s -o %t.o
# RUN: ld.lld --emit-relocs -e _start %t.o -o %t.exe

## The compact aligner requests only four-byte alignment without a profile.
# RUN: llvm-bolt %t.exe -o %t.classic --lite=0
# RUN: llvm-bolt %t.exe -o %t.compact --lite=0 --turn-off-stubs \
# RUN:   --verify-branch26-range | FileCheck %s --check-prefix=COMPACT
# COMPACT: JITLink layout: section=.text emitted-bytes=44 guard-bytes=0 stub-bytes=0 alignment-bytes=0 total-bytes=44
# RUN: llvm-nm -n %t.classic > %t.classic.nm
# RUN: llvm-nm -n %t.compact > %t.compact.nm
# RUN: diff %t.classic.nm %t.compact.nm

## With a 32-byte budget, skip padding of 60/56 bytes, but accept 28 bytes.
# RUN: llvm-bolt %t.exe -o %t.classic.bounded --lite=0 --use-compact-aligner=0
# RUN: llvm-bolt %t.exe -o %t.bounded --lite=0 --use-compact-aligner=0 \
# RUN:   --turn-off-stubs --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=BOUNDED
# BOUNDED: JITLink layout: section=.text emitted-bytes=44 guard-bytes=0 stub-bytes=0 alignment-bytes=28 total-bytes=72
# RUN: llvm-nm -n %t.classic.bounded > %t.classic.bounded.nm
# RUN: llvm-nm -n %t.bounded > %t.bounded.nm
# RUN: diff %t.classic.bounded.nm %t.bounded.nm

## A zero budget disables preferred alignment instead of requesting unlimited
## padding (the latter is the MC API's meaning of MaxBytesToEmit=0).
# RUN: llvm-bolt %t.exe -o %t.zero --lite=0 --use-compact-aligner=0 \
# RUN:   --align-functions-max-bytes=0 --turn-off-stubs --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=COMPACT
# RUN: llvm-nm -n %t.zero > %t.zero.nm
# RUN: diff %t.classic.nm %t.zero.nm

## The body's MC alignment remains mandatory even when entry padding is
## disabled. The constant island must still be eight-byte aligned.
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %t/island.s -o %t.ci.o
# RUN: ld.lld --emit-relocs -e _start %t.ci.o -o %t.ci.exe
# RUN: llvm-bolt %t.ci.exe -o %t.ci.bolt --lite=0 --turn-off-stubs \
# RUN:   --align-functions-max-bytes=0 --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=ISLAND
# ISLAND: JITLink layout: section=.text emitted-bytes=28 guard-bytes=0 stub-bytes=0 alignment-bytes=4 total-bytes=32
# RUN: llvm-nm -n %t.ci.bolt | FileCheck %s --check-prefix=CI-ENTRY
# CI-ENTRY: {{[0-9a-f]*[08]}} T with_island
# RUN: llvm-objdump -d --section=.text --disassemble-symbols='$d' %t.ci.bolt \
# RUN:   | FileCheck %s --check-prefix=CI-DATA
# CI-DATA: {{[0-9a-f]*[08]}} <$d>:

## Profiled split functions use their cold byte budget, independently of the
## main fragment's budget. Two eight-byte cold fragments need no padding.
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %t/split.s -o %t.split.o
# RUN: ld.lld --emit-relocs -e _start %t.split.o -o %t.split.exe
# RUN: link_fdata --no-lbr %s %t.split.exe %t.fdata
# RUN: llvm-strip --strip-unneeded %t.split.exe
# RUN: llvm-bolt %t.split.exe -o %t.split.bolt --lite=0 --turn-off-stubs \
# RUN:   --data=%t.fdata --split-functions --verify-branch26-range --print-stub-stats \
# RUN:   | FileCheck %s --check-prefix=COLD
# COLD: JITLink layout: section=.text.cold emitted-bytes=16 guard-bytes=0 stub-bytes=0 alignment-bytes=0 total-bytes=16
## Cross-section branches still reserve slots before section bases are known.
# COLD: JITLink stubs: slots=2 unused=2 short-adrp=0 long-mov=0 shared=0
# RUN: llvm-nm -n %t.split.bolt | FileCheck %s --check-prefix=COLD-NM
# COLD-NM: [[#%x,COLD_BASE:]] t _start.cold.0
# COLD-NM-NEXT: {{0*}}[[#COLD_BASE + 8]] t second.cold.0

#--- functions.s
.text
.globl _start
.type _start, %function
_start:
  b first
.size _start, .-_start

.globl first
.type first, %function
first:
  ret
.size first, .-first

.globl second
.type second, %function
second:
  .rept 6
  add x0, x0, #1
  .endr
  ret
.size second, .-second

.globl third
.type third, %function
third:
  ret
.size third, .-third

.globl fourth
.type fourth, %function
fourth:
  b _start
.size fourth, .-fourth

#--- island.s
.text
.globl _start
.type _start, %function
_start:
  b with_island
.size _start, .-_start

.globl with_island
.type with_island, %function
with_island:
  adrp x0, .Lci
  ldr x0, [x0, :lo12:.Lci]
  ret
.size with_island, .-with_island
  nop
  .p2align 3
.Lci:
  .xword _start

#--- split.s
.text
.globl _start
.type _start, %function
_start:
.start_hot:
# FDATA: 1 _start #.start_hot# 100
  cbz x0, .start_cold
.start_hotret:
# FDATA: 1 _start #.start_hotret# 100
  .rept 6
  add x0, x0, #1
  .endr
  ret
.start_cold:
  mov x0, #0
  ret
.size _start, .-_start

.globl second
.type second, %function
second:
.second_hot:
# FDATA: 1 second #.second_hot# 100
  cbz x0, .second_cold
.second_hotret:
# FDATA: 1 second #.second_hotret# 100
  .rept 6
  add x0, x0, #1
  .endr
  ret
.second_cold:
  mov x0, #0
  ret
.size second, .-second

.reloc 0, R_AARCH64_NONE
