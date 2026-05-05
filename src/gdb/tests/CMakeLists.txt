add_executable(gdb-test gdb-test.cpp)
target_link_libraries(gdb-test PRIVATE silk-fibers)

find_program(GDB_EXECUTABLE gdb)
if(NOT GDB_EXECUTABLE)
    message(STATUS "GDB not found -- skipping gdb-fiber-py test")
elseif(SANITIZER STREQUAL "memory")
    message(STATUS "MSan build -- skipping gdb-fiber-py test (MSan changes stack layouts that fiber.py inspects)")
else()
    set(_FIBER_PY    "${CMAKE_SOURCE_DIR}/src/gdb/fiber.py")
    set(_AUTOTEST_PY "${CMAKE_SOURCE_DIR}/src/gdb/tests/autotest.py")

    # Catch wrong paths at configure time rather than silently at test time.
    foreach(_script IN ITEMS "${_FIBER_PY}" "${_AUTOTEST_PY}")
        if(NOT EXISTS "${_script}")
            message(FATAL_ERROR "GDB script not found: ${_script}")
        endif()
    endforeach()

    # taskset limits FiberScheduler to 4 CPUs so GDB only tracks ~8 threads
    # instead of one per online CPU.  Without it, _walk_thread_fibers() in
    # fiber.py switches to every thread (~0.2 s each) and the test times out
    # on high-CPU-count machines.
    find_program(TASKSET_EXECUTABLE taskset)
    if(TASKSET_EXECUTABLE)
        set(_GDB_PREFIX ${TASKSET_EXECUTABLE} -c 0-3)
    else()
        set(_GDB_PREFIX "")
        message(STATUS "taskset not found -- gdb-fiber-py may be slow on many-CPU machines")
    endif()

    add_test(
        NAME gdb-fiber-py
        COMMAND ${_GDB_PREFIX} ${GDB_EXECUTABLE} --batch -q
                -ex "source ${_FIBER_PY}"
                -ex "source ${_AUTOTEST_PY}"
                $<TARGET_FILE:gdb-test>
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(gdb-fiber-py PROPERTIES
        TIMEOUT 60
        RESOURCE_LOCK iouring
    )
endif()
