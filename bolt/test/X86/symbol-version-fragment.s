## Verify that GNU symbol versions disambiguate duplicate global symbols
## without renaming unrelated aliases at the same addresses, and that an
## unversioned cold fragment is assigned using direct control flow rather than
## the default symbol version.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux %s -o %t.o
# RUN: echo 'VERS_1 { global: foo; foo_v1; };' > %t.map
# RUN: echo 'VERS_2 { global: foo; foo_v2; } VERS_1;' >> %t.map
# RUN: ld.lld %t.o -o %t.so -shared -Bsymbolic --version-script %t.map
# RUN: llvm-bolt %t.so -o %t.bolt -v=1 2>&1 | FileCheck %s
# RUN: llvm-readelf --symbols %t.bolt | FileCheck %s --check-prefix=SYMS

# CHECK-NOT: global symbol "foo" is not unique
# CHECK: BOLT-INFO: marking foo.cold as a fragment of foo_v1@@VERS_1
# CHECK-NOT: BOLT-INFO: marking foo.cold as a fragment of foo_v2@@VERS_2

# SYMS: foo_v1
# SYMS: foo_v2

.text
.globl foo_v1
.type foo_v1, @function
foo_v1:
  jne foo.cold
  ret
.size foo_v1, .-foo_v1

.globl foo_v2
.type foo_v2, @function
foo_v2:
  ret
.size foo_v2, .-foo_v2

.section .text.cold,"ax",@progbits
.globl foo.cold
.type foo.cold, @function
foo.cold:
  jmp foo_v1
.size foo.cold, .-foo.cold

.symver foo_v1, foo@VERS_1
.symver foo_v2, foo@@VERS_2
