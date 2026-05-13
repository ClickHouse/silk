// fcontext - minimal context-switching primitives.
//
// Implementation copied verbatim from Boost.Context (BSL-1.0). Only the two
// functions silk needs - make_fcontext and jump_fcontext - are vendored,
// along with the corresponding asm sources for x86_64-sysv and arm64-aapcs.
// See contrib/fcontext/README.md and contrib/fcontext/LICENSE_1_0.txt.

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void * fcontext_t;

typedef struct transfer_t
{
    fcontext_t fctx;
    void * data;
} transfer_t;

fcontext_t make_fcontext(void * sp, size_t size, void (* fn)(transfer_t));
transfer_t jump_fcontext(fcontext_t to, void * vp);

#ifdef __cplusplus
}
#endif
