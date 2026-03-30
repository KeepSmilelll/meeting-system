cmake_minimum_required(VERSION 3.20)
project(meeting_sfu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(sfu_core
    rtp/RtpParser.cpp
    rtp/NackBuffer.cpp
    rtp/RtcpHandler.cpp
    rtp/RtpRouter.cpp
)

target_include_directories(sfu_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(Boost QUIET COMPONENTS system)
if(Boost_FOUND)
    add_library(sfu_network
        server/UdpServer.cpp
    )

    target_include_directories(sfu_network
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(sfu_network
        PUBLIC
            Boost::system
            sfu_core
    )

    if(WIN32)
        target_link_libraries(sfu_network PUBLIC ws2_32 mswsock)
    endif()
else()
    message(WARNING "Boost::system not found; 'sfu_network' target will be skipped.")
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
