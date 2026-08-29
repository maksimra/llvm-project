## Direct branches between possible versioned parents and a fragment can be
## tail calls and must not be used as evidence for fragment ownership. Cover
## both an unconditional jump and a conditional branch to the fragment.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux %s -o %t.o
# RUN: echo 'VERS_1 { global: foo; foo_v1; };' > %t.map
# RUN: echo 'VERS_2 { global: foo; foo_v2; } VERS_1;' >> %t.map
# RUN: ld.lld %t.o -o %t.so -shared -Bsymbolic --version-script %t.map
# RUN: not llvm-bolt %t.so -o %t.bolt 2>&1 | FileCheck %s

# CHECK: BOLT-ERROR: unable to determine parent for fragment foo.cold;
# CHECK-SAME: possible versioned parents:

.text
.globl foo_v1
.type foo_v1, @function
foo_v1:
  ret
.size foo_v1, .-foo_v1

.globl foo_v2
.type foo_v2, @function
foo_v2:
  test %eax, %eax
  jne foo.cold
  ret
.size foo_v2, .-foo_v2

.section .text.cold,"ax",@progbits
.globl foo.cold
.type foo.cold, @function
foo.cold:
  jmp foo_v2
.size foo.cold, .-foo.cold

.symver foo_v1, foo@VERS_1
.symver foo_v2, foo@@VERS_2
