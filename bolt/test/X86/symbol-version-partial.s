## Version-based disambiguation is all-or-nothing for a conflicting raw name.
## Globalize a linker-localized second foo after linking to construct partial
## dynamic version coverage deterministically.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux %s -o %t.o
# RUN: echo 'VERS_1 { global: foo; foo_v1; };' > %t.map
# RUN: echo 'VERS_2 { global: foo; } VERS_1;' >> %t.map
# RUN: ld.lld %t.o -o %t.so -shared -Bsymbolic --version-script %t.map
# RUN: llvm-objcopy --globalize-symbol=foo %t.so %t.partial.so
# RUN: llvm-readelf --symbols %t.partial.so | FileCheck %s --check-prefix=INPUT
# RUN: not llvm-bolt %t.partial.so -o %t.bolt 2>&1 | FileCheck %s --check-prefix=BOLT

# INPUT-LABEL: Symbol table '.dynsym'
# INPUT: foo@@VERS_1
# INPUT-NOT: foo@VERS_2
# INPUT-LABEL: Symbol table '.symtab'
# INPUT: [[B:[0-9a-f]+]]     2 FUNC {{.*}} foo
# INPUT: [[A:[0-9a-f]+]]     1 FUNC {{.*}} foo
# BOLT: BOLT-ERROR: bad input binary, global symbol "foo" is not unique

.text
.globl foo_v1
.type foo_v1, @function
foo_v1:
  ret
.size foo_v1, .-foo_v1

.globl foo_v2
.hidden foo_v2
.type foo_v2, @function
foo_v2:
  nop
  ret
.size foo_v2, .-foo_v2

.section .text.cold,"ax",@progbits
.globl foo.cold
.type foo.cold, @function
foo.cold:
  ret
.size foo.cold, .-foo.cold

.symver foo_v1, foo@@VERS_1
.symver foo_v2, foo@VERS_2
