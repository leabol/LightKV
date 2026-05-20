cmake_minimum_required(VERSION 3.16)

project(LightKV LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LIGHTKV_BUILD_EXAMPLES "Build LightKV examples" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(net STATIC
    src/net/Acceptor.cpp
    src/net/Channel.cpp
    src/net/EpollPoller.cpp
    src/net/EventLoop.cpp
    src/net/buffer.cpp
    src/net/Socket.cpp
    src/net/TcpConnection.cpp
    src/net/TcpServer.cpp
    src/protocol/parser.cpp
    src/protocol/codec.cpp
    src/server/kv_server.cpp
    src/net/EventLoopThread.cpp
)

target_include_directories(net
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/net
        ${CMAKE_CURRENT_SOURCE_DIR}/src/protocol
        ${CMAKE_CURRENT_SOURCE_DIR}/src/server
        ${CMAKE_CURRENT_SOURCE_DIR}/src/util
)

add_library(util STATIC
    src/util/Log.cpp
)

target_include_directories(util
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/util
)

target_compile_options(net PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(util PRIVATE -Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)
find_package(spdlog CONFIG REQUIRED)

target_link_libraries(net
    PUBLIC
        Threads::Threads
        spdlog::spdlog
    util

)

if(LIGHTKV_BUILD_EXAMPLES)
    add_executable(lightkv_server_demo
        examples/server_demo.cpp
    )
    target_link_libraries(lightkv_server_demo PRIVATE net util)

    add_executable(lightkv_client_demo
        examples/client_demo.cpp
    )
    target_link_libraries(lightkv_client_demo PRIVATE net util)
endif()

# Build the main KvServer executable from src/main.cpp
add_executable(lightkv_kv_server
    src/main.cpp
)
target_link_libraries(lightkv_kv_server PRIVATE net util)

    add_executable(lightkv_protocol_client
        examples/protocol_client.cpp
    )
    target_link_libraries(lightkv_protocol_client PRIVATE net util)

    add_executable(lightkv_load_test_client
        tests/load_test_client.cpp
    )
    target_link_libraries(lightkv_load_test_client PRIVATE net util)

    add_executable(lightkv_rbench_client
        tests/rbench_client.cpp
    )
    target_link_libraries(lightkv_rbench_client PRIVATE net util)
add_executable(lightkv_conn_stress_test
    tests/conn_stress_test.cpp
)
target_link_libraries(lightkv_conn_stress_test PRIVATE net util)