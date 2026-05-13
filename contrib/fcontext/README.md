# fcontext

A minimal vendor of Boost.Context's `fcontext` primitives - just enough to
implement cooperative context switching for silk's fiber scheduler without
pulling in the full Boost.Context library.

## What's here

- `fcontext.h` - C declarations of `make_fcontext` and `jump_fcontext`.
- `asm/jump_x86_64_sysv_elf_gas.S`
- `asm/make_x86_64_sysv_elf_gas.S`
- `asm/jump_arm64_aapcs_elf_gas.S`
- `asm/make_arm64_aapcs_elf_gas.S`

## Provenance

The asm files are copied unmodified from Boost.Context. Copyright notices and
the Boost Software License 1.0 header are preserved at the top of each file.

Upstream:

- https://github.com/boostorg/context/blob/develop/src/asm/jump_x86_64_sysv_elf_gas.S
- https://github.com/boostorg/context/blob/develop/src/asm/make_x86_64_sysv_elf_gas.S
- https://github.com/boostorg/context/blob/develop/src/asm/jump_arm64_aapcs_elf_gas.S
- https://github.com/boostorg/context/blob/develop/src/asm/make_arm64_aapcs_elf_gas.S

Original authors: Oliver Kowalke (x86_64, 2009), Edward Nevill + Oliver
Kowalke (arm64, 2015).

Licensed under the Boost Software License 1.0 - see `LICENSE_1_0.txt`.

## Why vendor instead of linking Boost.Context?

The full Boost.Context library is several MB and pulls in a system Boost
runtime. silk uses only two symbols from it (`make_fcontext`, `jump_fcontext`)
on two architectures (x86_64-sysv-elf, arm64-aapcs-elf). Vendoring the four
asm sources keeps silk self-contained and statically linkable without any
system Boost runtime dependency.

`ontop_fcontext` (a third boost::context primitive) is not used by silk and
is not vendored.
