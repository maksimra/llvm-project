## Verify that GNU symbol versions disambiguate duplicate global symbols
## without renaming unrelated aliases at the same addresses.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux %s -o %t.o
# RUN: echo 'VERS_1 { global: foo; foo_v1; };' > %t.map
# RUN: echo 'VERS_2 { global: foo; foo_v2; } VERS_1;' >> %t.map
# RUN: ld.lld %t.o -o %t.so -shared -Bsymbolic --version-script %t.map
# RUN: llvm-bolt %t.so -o %t.bolt -v=1 2>&1 | FileCheck %s
# RUN: llvm-readelf --symbols %t.bolt | FileCheck %s --check-prefixes=VERS,SYMS

# CHECK-NOT: global symbol "foo" is not unique
# VERS-LABEL: Symbol table '.dynsym'
# VERS-DAG: foo@VERS_1
# VERS-DAG: foo@@VERS_2
# SYMS-LABEL: Symbol table '.symtab'
# SYMS: [[V1:[0-9a-f]+]] {{.*}} foo_v1
# SYMS: [[V2:[0-9a-f]+]] {{.*}} foo_v2
# SYMS: [[V1]] {{.*}} foo
# SYMS: [[V2]] {{.*}} foo

.text
.globl foo_v1
.type foo_v1, @function
foo_v1:
  ret
.size foo_v1, .-foo_v1

.globl foo_v2
.type foo_v2, @function
foo_v2:
  ret
.size foo_v2, .-foo_v2

.symver foo_v1, foo@VERS_1
.symver foo_v2, foo@@VERS_2
