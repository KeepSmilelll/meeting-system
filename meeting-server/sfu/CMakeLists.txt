cmake_minimum_required(VERSION 3.20)
project(meeting_sfu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()

option(SFU_BUILD_NETWORK "Build the UDP network target" ON)

add_library(sfu_core
    rtp/RtpParser.cpp
    rtp/NackBuffer.cpp
    rtp/RtcpHandler.cpp
    rtp/RtpRouter.cpp
    room/Room.cpp
    room/RoomManager.cpp
    room/Publisher.cpp
    room/Subscriber.cpp
)

target_include_directories(sfu_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

add_library(sfu_network
    server/UdpServer.cpp
)

target_include_directories(sfu_network
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sfu_network
    PUBLIC
        sfu_core
)

if(WIN32)
    target_link_libraries(sfu_network PUBLIC ws2_32)
endif()

add_library(sfu_rpc_server_core
    server/MediaIngress.cpp
    server/RpcProtocol.cpp
    server/RpcService.cpp
    server/RpcServer.cpp
)

target_include_directories(sfu_rpc_server_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sfu_rpc_server_core
    PUBLIC
        sfu_core
        sfu_network
)

if(WIN32)
    target_link_libraries(sfu_rpc_server_core PUBLIC ws2_32)
endif()

add_executable(sfu_core_tests
    tests/sfu_core_tests.cpp
)

target_link_libraries(sfu_core_tests
    PRIVATE
        sfu_core
)

enable_testing()
add_test(NAME sfu_core_tests COMMAND sfu_core_tests)

add_executable(sfu_rpc_server_tests
    tests/sfu_rpc_server_tests.cpp
)

target_link_libraries(sfu_rpc_server_tests
    PRIVATE
        sfu_rpc_server_core
)

if(WIN32)
    target_link_libraries(sfu_rpc_server_tests PRIVATE ws2_32)
endif()

add_test(NAME sfu_rpc_server_tests COMMAND sfu_rpc_server_tests)
