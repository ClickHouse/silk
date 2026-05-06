set(LIBRSEQ_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../librseq)

add_library(librseq STATIC
    ${LIBRSEQ_SRC}/src/rseq.c
    ${LIBRSEQ_SRC}/src/smp.c
)

target_include_directories(librseq
    PUBLIC  ${LIBRSEQ_SRC}/include
    PRIVATE ${LIBRSEQ_SRC}/src
)

target_compile_definitions(librseq PRIVATE _GNU_SOURCE)

target_compile_options(librseq PRIVATE -Wno-unused-parameter -Wno-deprecated-volatile)

add_library(Librseq::Librseq ALIAS librseq)
