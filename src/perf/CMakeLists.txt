add_library(silk-perf STATIC common.cpp)
target_link_libraries(silk-perf PUBLIC silk-fibers)

add_executable(file-perf file-perf.cpp)
target_link_libraries(file-perf PRIVATE silk-perf Boost::program_options)

add_executable(net-perf net-perf.cpp)
target_link_libraries(net-perf PRIVATE silk-perf Boost::program_options)

add_executable(net-perf-epoll net-perf-epoll.cpp)
target_link_libraries(net-perf-epoll PRIVATE silk-perf Boost::program_options)

# asio::awaitable requires Boost >= 1.75
if(Boost_VERSION_STRING VERSION_GREATER_EQUAL "1.75")
    add_executable(net-perf-asio net-perf-asio.cpp)
    target_link_libraries(net-perf-asio PRIVATE silk-perf Boost::program_options)

    if(BUILD_JEMALLOC)
        target_link_libraries(net-perf-asio PRIVATE jemalloc::jemalloc)
    endif()
endif()

if(BUILD_POCO)
    add_library(silk-perf-http STATIC fiber-http.cpp)
    target_link_libraries(silk-perf-http PUBLIC silk-perf Poco::Net Poco::Foundation)

    add_executable(http-perf http-perf.cpp)
    target_link_libraries(http-perf PRIVATE silk-perf-http Boost::program_options)

    if(BUILD_JEMALLOC)
        target_link_libraries(http-perf PRIVATE jemalloc::jemalloc)
    endif()
endif()

if(BUILD_AWS)
    add_executable(s3-perf s3-perf.cpp)
    target_link_libraries(s3-perf PRIVATE silk-perf-http Boost::program_options aws-cpp-sdk-s3 aws-cpp-sdk-core)

    if(BUILD_JEMALLOC)
        target_link_libraries(s3-perf PRIVATE jemalloc::jemalloc)
    endif()
endif()
