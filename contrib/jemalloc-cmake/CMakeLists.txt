set(JEMALLOC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../jemalloc")

set(SRCS
    "${JEMALLOC_SOURCE_DIR}/src/arena.c"
    "${JEMALLOC_SOURCE_DIR}/src/background_thread.c"
    "${JEMALLOC_SOURCE_DIR}/src/base.c"
    "${JEMALLOC_SOURCE_DIR}/src/bin.c"
    "${JEMALLOC_SOURCE_DIR}/src/bin_info.c"
    "${JEMALLOC_SOURCE_DIR}/src/bitmap.c"
    "${JEMALLOC_SOURCE_DIR}/src/buf_writer.c"
    "${JEMALLOC_SOURCE_DIR}/src/cache_bin.c"
    "${JEMALLOC_SOURCE_DIR}/src/ckh.c"
    "${JEMALLOC_SOURCE_DIR}/src/counter.c"
    "${JEMALLOC_SOURCE_DIR}/src/ctl.c"
    "${JEMALLOC_SOURCE_DIR}/src/decay.c"
    "${JEMALLOC_SOURCE_DIR}/src/div.c"
    "${JEMALLOC_SOURCE_DIR}/src/ecache.c"
    "${JEMALLOC_SOURCE_DIR}/src/edata.c"
    "${JEMALLOC_SOURCE_DIR}/src/edata_cache.c"
    "${JEMALLOC_SOURCE_DIR}/src/ehooks.c"
    "${JEMALLOC_SOURCE_DIR}/src/emap.c"
    "${JEMALLOC_SOURCE_DIR}/src/eset.c"
    "${JEMALLOC_SOURCE_DIR}/src/exp_grow.c"
    "${JEMALLOC_SOURCE_DIR}/src/extent.c"
    "${JEMALLOC_SOURCE_DIR}/src/extent_dss.c"
    "${JEMALLOC_SOURCE_DIR}/src/extent_mmap.c"
    "${JEMALLOC_SOURCE_DIR}/src/fxp.c"
    "${JEMALLOC_SOURCE_DIR}/src/hook.c"
    "${JEMALLOC_SOURCE_DIR}/src/hpa.c"
    "${JEMALLOC_SOURCE_DIR}/src/hpa_hooks.c"
    "${JEMALLOC_SOURCE_DIR}/src/hpdata.c"
    "${JEMALLOC_SOURCE_DIR}/src/inspect.c"
    "${JEMALLOC_SOURCE_DIR}/src/jemalloc.c"
    "${JEMALLOC_SOURCE_DIR}/src/large.c"
    "${JEMALLOC_SOURCE_DIR}/src/log.c"
    "${JEMALLOC_SOURCE_DIR}/src/malloc_io.c"
    "${JEMALLOC_SOURCE_DIR}/src/mutex.c"
    "${JEMALLOC_SOURCE_DIR}/src/nstime.c"
    "${JEMALLOC_SOURCE_DIR}/src/pa.c"
    "${JEMALLOC_SOURCE_DIR}/src/pac.c"
    "${JEMALLOC_SOURCE_DIR}/src/pa_extra.c"
    "${JEMALLOC_SOURCE_DIR}/src/pages.c"
    "${JEMALLOC_SOURCE_DIR}/src/pai.c"
    "${JEMALLOC_SOURCE_DIR}/src/peak_event.c"
    "${JEMALLOC_SOURCE_DIR}/src/prof.c"
    "${JEMALLOC_SOURCE_DIR}/src/prof_data.c"
    "${JEMALLOC_SOURCE_DIR}/src/prof_log.c"
    "${JEMALLOC_SOURCE_DIR}/src/prof_recent.c"
    "${JEMALLOC_SOURCE_DIR}/src/prof_stats.c"
    "${JEMALLOC_SOURCE_DIR}/src/prof_sys.c"
    "${JEMALLOC_SOURCE_DIR}/src/psset.c"
    "${JEMALLOC_SOURCE_DIR}/src/rtree.c"
    "${JEMALLOC_SOURCE_DIR}/src/safety_check.c"
    "${JEMALLOC_SOURCE_DIR}/src/san_bump.c"
    "${JEMALLOC_SOURCE_DIR}/src/san.c"
    "${JEMALLOC_SOURCE_DIR}/src/sc.c"
    "${JEMALLOC_SOURCE_DIR}/src/sec.c"
    "${JEMALLOC_SOURCE_DIR}/src/stats.c"
    "${JEMALLOC_SOURCE_DIR}/src/sz.c"
    "${JEMALLOC_SOURCE_DIR}/src/tcache.c"
    "${JEMALLOC_SOURCE_DIR}/src/test_hooks.c"
    "${JEMALLOC_SOURCE_DIR}/src/thread_event.c"
    "${JEMALLOC_SOURCE_DIR}/src/ticker.c"
    "${JEMALLOC_SOURCE_DIR}/src/tsd.c"
    "${JEMALLOC_SOURCE_DIR}/src/witness.c"
)

add_library(_jemalloc STATIC ${SRCS})

target_include_directories(_jemalloc SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(_jemalloc PRIVATE
    "${JEMALLOC_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_compile_definitions(_jemalloc PRIVATE JEMALLOC_NO_PRIVATE_NAMESPACE)
target_compile_options(_jemalloc PRIVATE
    -D_GNU_SOURCE
    -Wno-redundant-decls
    -Wno-ignored-attributes)

add_library(jemalloc::jemalloc ALIAS _jemalloc)
