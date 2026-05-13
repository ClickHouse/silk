if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    set(FCONTEXT_ASM
        ${CMAKE_CURRENT_SOURCE_DIR}/asm/jump_x86_64_sysv_elf_gas.S
        ${CMAKE_CURRENT_SOURCE_DIR}/asm/make_x86_64_sysv_elf_gas.S
    )
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    set(FCONTEXT_ASM
        ${CMAKE_CURRENT_SOURCE_DIR}/asm/jump_arm64_aapcs_elf_gas.S
        ${CMAKE_CURRENT_SOURCE_DIR}/asm/make_arm64_aapcs_elf_gas.S
    )
else()
    message(FATAL_ERROR "fcontext: unsupported architecture ${CMAKE_SYSTEM_PROCESSOR}")
endif()

enable_language(ASM)

add_library(fcontext STATIC ${FCONTEXT_ASM})
target_include_directories(fcontext SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_library(Fcontext::Fcontext ALIAS fcontext)
