## Multiple versions of the same raw symbol may share an address. Preserve
## both versioned names as aliases of one function without renaming the
## unrelated implementation symbol at that address.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux %s -o %t.o
# RUN: echo 'VERS_1 { global: foo; };' > %t.map
# RUN: echo 'VERS_2 { global: foo; } VERS_1;' >> %t.map
# RUN: ld.lld %t.o -o %t.so -shared --version-script %t.map
# RUN: llvm-bolt %t.so -o %t.bolt
# RUN: llvm-readelf --symbols %t.bolt | FileCheck %s

# CHECK-DAG: foo_impl
# CHECK-DAG: foo@VERS_1
# CHECK-DAG: foo@@VERS_2

.text
.globl foo_impl
.type foo_impl, @function
foo_impl:
  ret
.size foo_impl, .-foo_impl

.symver foo_impl, foo@VERS_1
.symver foo_impl, foo@@VERS_2
