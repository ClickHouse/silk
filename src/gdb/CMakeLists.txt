# Install the gdb dump scripts next to every binary (all link into CMAKE_RUNTIME_OUTPUT_DIRECTORY):
# installCrashDumper resolves crash-dumper.py at run time from /proc/self/exe, and crash-dumper.py
# sources fiber.py from its own directory. Editing either triggers a reconfigure that re-copies it.
configure_file(crash-dumper.py ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/crash-dumper.py COPYONLY)
configure_file(fiber.py ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fiber.py COPYONLY)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
