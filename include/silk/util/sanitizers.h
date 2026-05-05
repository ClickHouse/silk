#pragma once

#if !defined(__SANITIZE_ADDRESS__) && defined(__has_feature) && __has_feature(address_sanitizer)
#    define __SANITIZE_ADDRESS__
#endif
#if defined(__SANITIZE_ADDRESS__)
#    include <sanitizer/asan_interface.h>
#endif

#if !defined(__SANITIZE_MEMORY__) && defined(__has_feature) && __has_feature(memory_sanitizer)
#    define __SANITIZE_MEMORY__
#endif
#if defined(__SANITIZE_MEMORY__)
#    include <sanitizer/msan_interface.h>
#    define MSAN_UNPOISON(ptr, size) __msan_unpoison(ptr, size)
#else
#    define MSAN_UNPOISON(ptr, size)
#endif

#if !defined(__SANITIZE_THREAD__) && defined(__has_feature) && __has_feature(thread_sanitizer)
#    define __SANITIZE_THREAD__
#endif
#if defined(__SANITIZE_THREAD__)
#    include <sanitizer/tsan_interface.h>
extern "C" {
void __tsan_ignore_thread_begin();
void __tsan_ignore_thread_end();
void * __tsan_get_current_fiber();
void * __tsan_create_fiber(unsigned flags);
void __tsan_destroy_fiber(void * fiber);
void __tsan_switch_to_fiber(void * fiber, unsigned flags);
}
#    define TSAN_ACQUIRE(addr) __tsan_acquire(addr)
#    define TSAN_RELEASE(addr) __tsan_release(addr)
#    define TSAN_IGNORE_BEGIN() __tsan_ignore_thread_begin()
#    define TSAN_IGNORE_END() __tsan_ignore_thread_end()
#    define TSAN_FIBER_CREATE() __tsan_create_fiber(0)
#    define TSAN_FIBER_DESTROY(fiber) __tsan_destroy_fiber(fiber)
#    define TSAN_FIBER_SWITCH(fiber) __tsan_switch_to_fiber(fiber, 0)
#    define TSAN_FIBER_GET_CURRENT() __tsan_get_current_fiber()
#else
#    define TSAN_ACQUIRE(addr)
#    define TSAN_RELEASE(addr)
#    define TSAN_IGNORE_BEGIN()
#    define TSAN_IGNORE_END()
#    define TSAN_FIBER_CREATE() nullptr
#    define TSAN_FIBER_DESTROY(fiber)
#    define TSAN_FIBER_SWITCH(fiber)
#    define TSAN_FIBER_GET_CURRENT() nullptr
#endif
