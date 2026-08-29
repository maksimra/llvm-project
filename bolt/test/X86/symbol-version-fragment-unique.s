## A sole genuine versioned parent is registered without requiring early
## control-flow evidence. The linker-created foo@PLT function is not a
## versioned candidate.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux %s -o %t.o
# RUN: echo 'VERS_1 { global: foo; foo_impl; };' > %t.map
# RUN: ld.lld %t.o -o %t.so -shared --version-script %t.map
# RUN: llvm-bolt %t.so -o %t.bolt -v=1 2>&1 | FileCheck %s

# CHECK: BOLT-INFO: marking foo.cold as a fragment of foo_impl@@VERS_1

.text
.globl foo_impl
.type foo_impl, @function
foo_impl:
  ret
.size foo_impl, .-foo_impl

.globl plt_user
.type plt_user, @function
plt_user:
  call foo@PLT
  ret
.size plt_user, .-plt_user

.section .text.cold,"ax",@progbits
.globl foo.cold
.type foo.cold, @function
foo.cold:
  ret
.size foo.cold, .-foo.cold

.symver foo_impl, foo@@VERS_1
