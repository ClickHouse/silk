#pragma once

#include <silk/util/platform.h>

#include <cstddef>

// librseq reads the thread pointer with __builtin_thread_pointer, which the compiler may keep in a
// register across a fiber switch; read it through getThreadPointer instead.
#define RSEQ_GET_ABI_OVERRIDE

// Suppress warnings emitted by librseq headers: volatile assignment in rseq_cs
// and unused parameters in the asm stubs.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-volatile"
#pragma clang diagnostic ignored "-Wunused-parameter"
#include <rseq/abi.h>

extern "C" ptrdiff_t rseq_offset;
extern "C" unsigned int rseq_size;

static inline struct rseq_abi * rseq_get_abi() noexcept
{
    return reinterpret_cast<struct rseq_abi *>(silk::getThreadPointer() + rseq_offset);
}

#include <rseq/rseq.h>
#pragma clang diagnostic pop
