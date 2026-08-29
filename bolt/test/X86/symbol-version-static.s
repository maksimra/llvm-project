## Version metadata is stripped when .symver definitions are linked into an
## ordinary static executable. With no reliable identity for the two raw foo
## symbols, BOLT must continue to reject the duplicate globals.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe
# RUN: llvm-readelf --version-info %t.exe | FileCheck %s --check-prefix=NOVERS --allow-empty
# RUN: not llvm-bolt %t.exe -o %t.bolt 2>&1 | FileCheck %s --check-prefix=BOLT

# NOVERS-NOT: Version symbols section
# BOLT: BOLT-ERROR: bad input binary, global symbol "foo" is not unique

.text
.globl _start
.type _start, @function
_start:
  call foo_v1
  call foo_v2
  ret
.size _start, .-_start

.globl foo_v1
.type foo_v1, @function
foo_v1:
  nop
  nop
.size foo_v1, .-foo_v1

.globl foo_v2
.type foo_v2, @function
foo_v2:
  ret
.size foo_v2, .-foo_v2

.symver foo_v1, foo@VERS_1
.symver foo_v2, foo@@VERS_2
