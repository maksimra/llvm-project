## A split function's hot and cold fragments become distinct semantic JITLink
## blocks, then return to their ordinary output sections without changing
## symbol sizes, addresses, or unwind ranges.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple=aarch64-linux %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -q --entry=_start
# RUN: link_fdata --no-lbr %s %t.exe %t.fdata
# RUN: llvm-bolt %t.exe -o %t.base --data %t.fdata --split-functions
# RUN: llvm-bolt %t.exe -o %t.semantic --data %t.fdata --split-functions \
# RUN:   --jitlink-branch26-relaxation --verify-branch26-range 2>&1 \
# RUN:   | FileCheck %s --check-prefix=SEMANTIC
# RUN: not llvm-bolt %t.exe -o %t.cdsplit --data %t.fdata --split-functions \
# RUN:   --split-strategy=cdsplit --jitlink-branch26-relaxation 2>&1 \
# RUN:   | FileCheck %s --check-prefix=CDSPLIT

# RUN: llvm-readelf -sW %t.base | grep ' FUNC ' \
# RUN:   | awk '{print $3, $8}' | sort > %t.base.funcs
# RUN: llvm-readelf -sW %t.semantic | grep ' FUNC ' \
# RUN:   | awk '{print $3, $8}' | sort > %t.semantic.funcs
# RUN: diff %t.base.funcs %t.semantic.funcs
# RUN: llvm-nm -nS --format=posix %t.semantic > %t.fdes
# RUN: llvm-dwarfdump --eh-frame %t.semantic >> %t.fdes
# RUN: FileCheck %s --input-file=%t.fdes --check-prefix=FDES
# RUN: llvm-readelf -SW %t.semantic | FileCheck %s --check-prefix=SECTIONS
## The existing final section-header merge also accepts the reconstructed
## ordinary .text/.text.cold graph sections.
# RUN: llvm-bolt %t.exe -o %t.merged --data %t.fdata --split-functions \
# RUN:   --jitlink-branch26-relaxation --merge-text-sections
# RUN: llvm-readelf -SW %t.merged | FileCheck %s --check-prefix=MERGED
## Padding before both fragments of chain makes the inter-fragment JUMP26
## exceed 128 MiB while leaving each post-batch group reachable.
# RUN: llvm-bolt %t.exe -o %t.far --data %t.fdata --split-functions \
# RUN:   --jitlink-branch26-relaxation --verify-branch26-range --lite=0 \
# RUN:   --pad-funcs-before=chain:134221824 2>&1 \
# RUN:   | FileCheck %s --check-prefix=FAR

# SEMANTIC: BOLT-INFO: JITLink Branch26 relaxation: batches=2, groups=2, reserved=2, used=0, direct=2, relaxed=0
# SEMANTIC: BOLT-INFO: AArch64 Branch26PCRel edges: total=2, out-of-range=0, CALL26=0, JUMP26=2
# CDSPLIT: BOLT-ERROR: CDSplit is not supported with LongJmp

## Hot and cold FDEs retain their logical 4- and 8-byte ranges.
# FDES: chain T [[#%x,HOT:]] 4
# FDES: chain.cold.0 t [[#%x,COLD:]] 8
# FDES-DAG: pc={{0*}}[[#%x,HOT]]...{{0*}}[[#%x,HOT+4]]
# FDES-DAG: pc={{0*}}[[#%x,COLD]]...{{0*}}[[#%x,COLD+8]]

# SECTIONS-DAG: .text             PROGBITS
# SECTIONS-DAG: .text.cold        PROGBITS
# SECTIONS-NOT: .bolt.jitlink

# MERGED: .text             PROGBITS
# MERGED-NOT: .text.cold
# MERGED-NOT: .bolt.jitlink

# FAR: BOLT-INFO: JITLink Branch26 relaxation: batches=2, groups=2, reserved=2, used=2, direct=0, relaxed=2
# FAR: BOLT-INFO: AArch64 Branch26PCRel edges: total=2, out-of-range=0, CALL26=0, JUMP26=2

        .text
        .globl  _start
        .type   _start, %function
_start:
        ret
        .size   _start, .-_start

        .globl  chain
        .type   chain, %function
chain:
.entry_bb:
# FDATA: 1 chain #.entry_bb# 100
        .cfi_startproc
        b       .Lcold_bb
.Lcold_bb:
        mov     w0, #1
        b       chain
        .cfi_endproc
        .size   chain, .-chain

## Leave room in the original text segment for separately emitted hot/cold
## output when BOLT uses lite mode for the unprofiled functions.
        .p2align 6
        .globl  filler
        .type   filler, %function
filler:
        .rept 32
        ret
        .endr
        .size filler, .-filler

## Force relocation mode even if all ordinary input relocations are consumed.
        .reloc 0, R_AARCH64_NONE
