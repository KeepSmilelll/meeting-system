cmake_minimum_required(VERSION 3.20)
project(meeting_sfu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()

option(SFU_BUILD_NETWORK "Build the UDP network target" ON)

set(Protobuf_USE_STATIC_LIBS ON)
find_package(Protobuf REQUIRED)

set(OPENSSL_ROOT_HINT "D:/LOL/英雄联盟(26)/Cross/coach/agent/lol_ai_coach/Python311/Library" CACHE PATH "Local OpenSSL 3 root hint")
set(LIBSRTP_ROOT_DEFAULT "D:/meeting/third_party/libsrtp2")
set(LIBSRTP_ROOT_HINT "${LIBSRTP_ROOT_DEFAULT}" CACHE PATH "Local libsrtp2 root hint")
if(LIBSRTP_ROOT_HINT STREQUAL "" AND EXISTS "${LIBSRTP_ROOT_DEFAULT}")
    set(LIBSRTP_ROOT_HINT "${LIBSRTP_ROOT_DEFAULT}" CACHE PATH "Local libsrtp2 root hint" FORCE)
endif()
find_path(OPENSSL_INCLUDE_DIR
    NAMES openssl/ssl.h
    HINTS "${OPENSSL_ROOT_HINT}/include"
)
find_library(OPENSSL_SSL_LIBRARY
    NAMES libssl ssl
    HINTS "${OPENSSL_ROOT_HINT}/lib"
)
find_library(OPENSSL_CRYPTO_LIBRARY
    NAMES libcrypto crypto
    HINTS "${OPENSSL_ROOT_HINT}/lib"
)
if(NOT (OPENSSL_INCLUDE_DIR AND OPENSSL_SSL_LIBRARY AND OPENSSL_CRYPTO_LIBRARY))
    message(FATAL_ERROR "meeting_sfu requires OpenSSL 3 headers/libs. Set OPENSSL_ROOT_HINT to an OpenSSL 3 install.")
endif()

unset(LIBSRTP_INCLUDE_DIR CACHE)
unset(LIBSRTP_LIBRARY CACHE)
find_path(LIBSRTP_INCLUDE_DIR
    NAMES srtp2/srtp.h srtp.h
    HINTS "${LIBSRTP_ROOT_HINT}/include" "${LIBSRTP_ROOT_HINT}/include/srtp2"
)
find_library(LIBSRTP_LIBRARY
    NAMES srtp2 libsrtp2 srtp libsrtp
    HINTS "${LIBSRTP_ROOT_HINT}/lib" "${LIBSRTP_ROOT_HINT}/lib64"
)
if(LIBSRTP_INCLUDE_DIR AND LIBSRTP_LIBRARY)
    message(STATUS "meeting_sfu found libsrtp2: ${LIBSRTP_LIBRARY}")
else()
    message(WARNING "meeting_sfu did not find libsrtp2. DTLS-SRTP milestone in PLAN.md remains blocked until LIBSRTP_ROOT_HINT is configured.")
endif()

set(OPENSSL_RUNTIME_DLLS)
if(WIN32 AND EXISTS "${OPENSSL_ROOT_HINT}/bin")
    foreach(openssl_dll_name IN ITEMS libssl-3-x64.dll libcrypto-3-x64.dll)
        set(openssl_dll_path "${OPENSSL_ROOT_HINT}/bin/${openssl_dll_name}")
        if(EXISTS "${openssl_dll_path}")
            list(APPEND OPENSSL_RUNTIME_DLLS "${openssl_dll_path}")
        endif()
    endforeach()
endif()

function(copy_openssl_runtime target_name)
    if(NOT WIN32)
        return()
    endif()
    foreach(openssl_dll IN LISTS OPENSSL_RUNTIME_DLLS)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${openssl_dll}"
                $<TARGET_FILE_DIR:${target_name}>
        )
    endforeach()
endfunction()

if(TARGET protobuf::libprotobuf)
    set(SFU_PROTOBUF_TARGET protobuf::libprotobuf)
elseif(TARGET Protobuf::libprotobuf)
    set(SFU_PROTOBUF_TARGET Protobuf::libprotobuf)
endif()


add_library(sfu_core
    bwe/BandwidthEstimator.cpp
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
    server/DtlsContext.cpp
    server/DtlsTransport.cpp
    server/MediaIngress.cpp
    server/RpcProtocol.cpp
    server/RpcService.cpp
    server/RpcServer.cpp
    server/SfuDaemon.cpp
    server/SrtpSession.cpp
    proto/pb/sfu_rpc.pb.cc
)

target_include_directories(sfu_rpc_server_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${OPENSSL_INCLUDE_DIR}
)
if(LIBSRTP_INCLUDE_DIR)
    target_include_directories(sfu_rpc_server_core PUBLIC ${LIBSRTP_INCLUDE_DIR})
endif()

target_link_libraries(sfu_rpc_server_core
    PUBLIC
        sfu_core
        sfu_network
        ${OPENSSL_SSL_LIBRARY}
        ${OPENSSL_CRYPTO_LIBRARY}
)
if(LIBSRTP_LIBRARY)
    target_link_libraries(sfu_rpc_server_core PUBLIC ${LIBSRTP_LIBRARY})
    target_compile_definitions(sfu_rpc_server_core PUBLIC HAVE_LIBSRTP2=1)
endif()

if(SFU_PROTOBUF_TARGET)
    target_link_libraries(sfu_rpc_server_core PUBLIC ${SFU_PROTOBUF_TARGET})
else()
    target_include_directories(sfu_rpc_server_core PUBLIC ${Protobuf_INCLUDE_DIRS})
    target_link_libraries(sfu_rpc_server_core PUBLIC ${Protobuf_LIBRARIES})
endif()

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
copy_openssl_runtime(sfu_rpc_server_tests)

add_executable(meeting_sfu
    main.cpp
)

target_link_libraries(meeting_sfu
    PRIVATE
        sfu_rpc_server_core
)

if(WIN32)
    target_link_libraries(meeting_sfu PRIVATE ws2_32)
endif()
copy_openssl_runtime(meeting_sfu)

add_executable(sfu_daemon_tests
    tests/sfu_daemon_tests.cpp
)

target_link_libraries(sfu_daemon_tests
    PRIVATE
        sfu_rpc_server_core
)

if(WIN32)
    target_link_libraries(sfu_daemon_tests PRIVATE ws2_32)
endif()

add_test(NAME sfu_daemon_tests COMMAND sfu_daemon_tests)
copy_openssl_runtime(sfu_daemon_tests)
