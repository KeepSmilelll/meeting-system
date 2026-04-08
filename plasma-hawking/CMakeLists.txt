cmake_minimum_required(VERSION 3.20)
project(meeting_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick Network Sql Multimedia)
include(CTest)

# Optional FFmpeg smoke target for the Opus audio pipeline.
option(MEETING_BUILD_AUDIO_CODEC_SMOKE "Build the audio codec smoke test target" ON)
set(FFMPEG_ROOT_HINT "D:/Setup/ffmpeg-7.1-full_build-shared" CACHE PATH "Local FFmpeg root hint")

# Keep protobuf runtime/toolchain aligned with locally provisioned OpenCV libs.
set(PROTOBUF_INCLUDE_DIR_HINT "D:/openCV/opencv-4.12.0/sources/3rdparty/protobuf/src" CACHE PATH "Local protobuf include dir hint")
set(PROTOBUF_LIBRARY_RELEASE_HINT "D:/openCV/opencv-4.12.0/opencv_build/3rdparty/lib/Release/libprotobuf.lib" CACHE FILEPATH "Local protobuf release library hint")
set(PROTOBUF_LIBRARY_DEBUG_HINT "D:/openCV/opencv-4.12.0/opencv_build/3rdparty/lib/Debug/libprotobufd.lib" CACHE FILEPATH "Local protobuf debug library hint")
set(PROTOBUF_PROTOC_HINT "D:/go-env/protoc-3.19.1/bin/protoc.exe" CACHE FILEPATH "Local protoc hint")

# Force static protobuf to avoid accidental dll-import model mismatches.
set(Protobuf_USE_STATIC_LIBS ON)
if(EXISTS "${PROTOBUF_INCLUDE_DIR_HINT}")
    set(Protobuf_INCLUDE_DIR "${PROTOBUF_INCLUDE_DIR_HINT}" CACHE PATH "Protobuf include dir" FORCE)
endif()
if(EXISTS "${PROTOBUF_LIBRARY_RELEASE_HINT}")
    set(Protobuf_LIBRARY_RELEASE "${PROTOBUF_LIBRARY_RELEASE_HINT}" CACHE FILEPATH "Protobuf release lib" FORCE)
    set(Protobuf_LIBRARY "${PROTOBUF_LIBRARY_RELEASE_HINT}" CACHE FILEPATH "Protobuf default lib" FORCE)
endif()
if(EXISTS "${PROTOBUF_LIBRARY_DEBUG_HINT}")
    set(Protobuf_LIBRARY_DEBUG "${PROTOBUF_LIBRARY_DEBUG_HINT}" CACHE FILEPATH "Protobuf debug lib" FORCE)
endif()
if(EXISTS "${PROTOBUF_PROTOC_HINT}")
    set(Protobuf_PROTOC_EXECUTABLE "${PROTOBUF_PROTOC_HINT}" CACHE FILEPATH "Protobuf protoc executable" FORCE)
endif()

unset(FFMPEG_INCLUDE_DIR CACHE)
unset(FFMPEG_AVCODEC_LIBRARY CACHE)
unset(FFMPEG_AVUTIL_LIBRARY CACHE)
unset(FFMPEG_SWRESAMPLE_LIBRARY CACHE)
unset(FFMPEG_SWSCALE_LIBRARY CACHE)
if(EXISTS "${FFMPEG_ROOT_HINT}")
    find_path(FFMPEG_INCLUDE_DIR
        NAMES libavcodec/avcodec.h
        HINTS "${FFMPEG_ROOT_HINT}/include"
        NO_DEFAULT_PATH
    )
    find_library(FFMPEG_AVCODEC_LIBRARY
        NAMES avcodec libavcodec
        HINTS "${FFMPEG_ROOT_HINT}/lib"
        NO_DEFAULT_PATH
    )
    find_library(FFMPEG_AVUTIL_LIBRARY
        NAMES avutil libavutil
        HINTS "${FFMPEG_ROOT_HINT}/lib"
        NO_DEFAULT_PATH
    )
    find_library(FFMPEG_SWRESAMPLE_LIBRARY
        NAMES swresample libswresample
        HINTS "${FFMPEG_ROOT_HINT}/lib"
        NO_DEFAULT_PATH
    )
    find_library(FFMPEG_SWSCALE_LIBRARY
        NAMES swscale libswscale
        HINTS "${FFMPEG_ROOT_HINT}/lib"
        NO_DEFAULT_PATH
    )
endif()

find_package(Protobuf REQUIRED)
qt_standard_project_setup()

if(NOT (FFMPEG_INCLUDE_DIR AND FFMPEG_AVCODEC_LIBRARY AND FFMPEG_AVUTIL_LIBRARY AND FFMPEG_SWRESAMPLE_LIBRARY AND FFMPEG_SWSCALE_LIBRARY))
    message(FATAL_ERROR "meeting_client requires FFmpeg 7.1 headers/libs under ${FFMPEG_ROOT_HINT}")
endif()
qt_add_executable(meeting_client
    src/main.cpp
    src/app/AppStateMachine.cpp
    src/app/MeetingController.cpp
    src/app/MediaSessionManager.cpp
    src/app/ParticipantListModel.cpp
    src/app/RuntimeSmokeDriver.cpp
    src/app/UserManager.cpp
    src/av/capture/AudioCapture.cpp
    src/av/capture/ScreenCapture.cpp
    src/av/codec/AudioEncoder.cpp
    src/av/codec/AudioDecoder.cpp
    src/av/codec/VideoDecoder.cpp
    src/av/codec/VideoEncoder.cpp
    src/av/render/AudioPlayer.cpp
    src/av/render/NV12Shader.cpp
    src/av/render/VideoFrameStore.cpp
    src/av/render/VideoRenderer.cpp
    src/av/session/AudioCallSession.cpp
    src/av/session/ScreenShareSession.cpp
    src/storage/DatabaseManager.cpp
    src/storage/SettingsRepository.cpp
    src/storage/UserRepository.cpp
    src/storage/MessageRepository.cpp
    src/storage/MeetingRepository.cpp
    src/storage/CallLogRepository.cpp
    src/net/signaling/SignalProtocol.cpp
    src/net/signaling/SignalingClient.cpp
    src/net/signaling/Reconnector.cpp
    src/net/media/RTPSender.cpp
    src/net/media/RTPReceiver.cpp
    src/net/media/JitterBuffer.cpp
    proto/pb/signaling.pb.cc
)

target_include_directories(meeting_client PRIVATE
    src
    proto/pb
    ${FFMPEG_INCLUDE_DIR}
)

target_link_libraries(meeting_client PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Network
    Qt6::Sql
    Qt6::Multimedia
    protobuf::libprotobuf
    ${FFMPEG_AVCODEC_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
    ${FFMPEG_SWRESAMPLE_LIBRARY}
    ${FFMPEG_SWSCALE_LIBRARY}
)

if(WIN32)
    target_link_libraries(meeting_client PRIVATE ws2_32 bcrypt)
endif()

qt_add_qml_module(meeting_client
    URI MeetingApp
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/dialogs/LoginDialog.qml
        qml/pages/HomePage.qml
        qml/pages/MeetingRoom.qml
        qml/components/CreateMeetingDialog.qml
        qml/components/TitleBar.qml
        qml/components/ToolBar.qml
        qml/components/ParticipantPanel.qml
)

add_executable(test_database
    tests/test_database.cpp
    src/storage/DatabaseManager.cpp
    src/storage/SettingsRepository.cpp
    src/storage/UserRepository.cpp
    src/storage/MessageRepository.cpp
    src/storage/MeetingRepository.cpp
    src/storage/CallLogRepository.cpp
)

target_include_directories(test_database PRIVATE
    src
)

target_link_libraries(test_database PRIVATE
    Qt6::Core
    Qt6::Sql
)

add_test(NAME test_database COMMAND test_database)

add_executable(test_media_session_manager
    tests/test_media_session_manager.cpp
    src/app/MediaSessionManager.cpp
)

target_include_directories(test_media_session_manager PRIVATE
    src
)

target_link_libraries(test_media_session_manager PRIVATE
    Qt6::Core
)

add_test(NAME test_media_session_manager COMMAND test_media_session_manager)

add_executable(test_screen_share_session
    tests/test_screen_share_session.cpp
    src/av/capture/ScreenCapture.cpp
    src/av/codec/VideoDecoder.cpp
    src/av/codec/VideoEncoder.cpp
    src/av/session/ScreenShareSession.cpp
    src/net/media/RTPSender.cpp
    src/net/media/RTPReceiver.cpp
)

target_include_directories(test_screen_share_session PRIVATE
    src
    ${FFMPEG_INCLUDE_DIR}
)

target_link_libraries(test_screen_share_session PRIVATE
    Qt6::Core
    Qt6::Network
    ${FFMPEG_AVCODEC_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
    ${FFMPEG_SWSCALE_LIBRARY}
)

if(WIN32)
    target_link_libraries(test_screen_share_session PRIVATE ws2_32)
endif()

add_test(NAME test_screen_share_session COMMAND test_screen_share_session)

add_executable(test_signaling_client_ssrc
    tests/test_signaling_client_ssrc.cpp
    src/net/signaling/SignalProtocol.cpp
    src/net/signaling/SignalingClient.cpp
    proto/pb/signaling.pb.cc
)

target_include_directories(test_signaling_client_ssrc PRIVATE
    src
    proto/pb
)

target_link_libraries(test_signaling_client_ssrc PRIVATE
    Qt6::Core
    Qt6::Network
    protobuf::libprotobuf
)

add_test(NAME test_signaling_client_ssrc COMMAND test_signaling_client_ssrc)

add_executable(test_meeting_controller_mute_all
    tests/test_meeting_controller_mute_all.cpp
    src/app/MeetingController.cpp
    src/app/MediaSessionManager.cpp
    src/app/ParticipantListModel.cpp
    src/app/UserManager.cpp
    src/av/capture/AudioCapture.cpp
    src/av/capture/ScreenCapture.cpp
    src/av/codec/AudioEncoder.cpp
    src/av/codec/AudioDecoder.cpp
    src/av/codec/VideoDecoder.cpp
    src/av/codec/VideoEncoder.cpp
    src/av/render/AudioPlayer.cpp
    src/av/render/VideoFrameStore.cpp
    src/av/session/AudioCallSession.cpp
    src/av/session/ScreenShareSession.cpp
    src/storage/DatabaseManager.cpp
    src/storage/SettingsRepository.cpp
    src/storage/MeetingRepository.cpp
    src/storage/CallLogRepository.cpp
    src/net/signaling/SignalProtocol.cpp
    src/net/signaling/SignalingClient.cpp
    src/net/signaling/Reconnector.cpp
    src/net/media/RTPSender.cpp
    src/net/media/RTPReceiver.cpp
    src/net/media/BandwidthEstimator.cpp
    src/net/media/JitterBuffer.cpp
    proto/pb/signaling.pb.cc
)

target_include_directories(test_meeting_controller_mute_all PRIVATE
    src
    proto/pb
    ${FFMPEG_INCLUDE_DIR}
)

target_link_libraries(test_meeting_controller_mute_all PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Multimedia
    protobuf::libprotobuf
    ${FFMPEG_AVCODEC_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
    ${FFMPEG_SWRESAMPLE_LIBRARY}
    ${FFMPEG_SWSCALE_LIBRARY}
)

if(WIN32)
    target_link_libraries(test_meeting_controller_mute_all PRIVATE ws2_32 bcrypt)
endif()

add_test(NAME test_meeting_controller_mute_all COMMAND test_meeting_controller_mute_all)

add_executable(test_meeting_runtime_smoke
    tests/test_meeting_runtime_smoke.cpp
    src/app/MeetingController.cpp
    src/app/MediaSessionManager.cpp
    src/app/ParticipantListModel.cpp
    src/app/UserManager.cpp
    src/av/capture/AudioCapture.cpp
    src/av/capture/ScreenCapture.cpp
    src/av/codec/AudioEncoder.cpp
    src/av/codec/AudioDecoder.cpp
    src/av/codec/VideoDecoder.cpp
    src/av/codec/VideoEncoder.cpp
    src/av/render/AudioPlayer.cpp
    src/av/render/VideoFrameStore.cpp
    src/av/session/AudioCallSession.cpp
    src/av/session/ScreenShareSession.cpp
    src/storage/DatabaseManager.cpp
    src/storage/SettingsRepository.cpp
    src/storage/MeetingRepository.cpp
    src/storage/CallLogRepository.cpp
    src/net/signaling/SignalProtocol.cpp
    src/net/signaling/SignalingClient.cpp
    src/net/signaling/Reconnector.cpp
    src/net/media/RTPSender.cpp
    src/net/media/RTPReceiver.cpp
    src/net/media/BandwidthEstimator.cpp
    src/net/media/JitterBuffer.cpp
    proto/pb/signaling.pb.cc
)

target_include_directories(test_meeting_runtime_smoke PRIVATE
    src
    proto/pb
    ${FFMPEG_INCLUDE_DIR}
)

target_link_libraries(test_meeting_runtime_smoke PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Multimedia
    protobuf::libprotobuf
    ${FFMPEG_AVCODEC_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
    ${FFMPEG_SWRESAMPLE_LIBRARY}
    ${FFMPEG_SWSCALE_LIBRARY}
)

if(WIN32)
    target_link_libraries(test_meeting_runtime_smoke PRIVATE ws2_32 bcrypt)
endif()

add_test(NAME test_meeting_runtime_smoke COMMAND test_meeting_runtime_smoke)

add_executable(test_meeting_client_process_smoke
    tests/test_meeting_client_process_smoke.cpp
)

target_link_libraries(test_meeting_client_process_smoke PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Multimedia
)

add_dependencies(test_meeting_client_process_smoke meeting_client)

add_test(NAME test_meeting_client_process_smoke COMMAND test_meeting_client_process_smoke)
add_test(NAME test_meeting_client_real_audio_smoke COMMAND test_meeting_client_process_smoke)
set_tests_properties(test_meeting_client_real_audio_smoke PROPERTIES
    ENVIRONMENT "MEETING_PROCESS_SMOKE_SYNTHETIC_AUDIO=0"
    SKIP_RETURN_CODE 77
)

if(MEETING_BUILD_AUDIO_CODEC_SMOKE)
    if(FFMPEG_INCLUDE_DIR AND FFMPEG_AVCODEC_LIBRARY AND FFMPEG_AVUTIL_LIBRARY AND FFMPEG_SWRESAMPLE_LIBRARY AND FFMPEG_SWSCALE_LIBRARY)
        add_executable(audio_codec_smoke
            tests/AudioCodecSmoke.cpp
            src/av/capture/AudioCapture.cpp
            src/av/codec/AudioEncoder.cpp
            src/av/codec/AudioDecoder.cpp
            src/av/render/AudioPlayer.cpp
            src/av/session/AudioCallSession.cpp
            src/net/media/RTPSender.cpp
            src/net/media/RTPReceiver.cpp
            src/net/media/BandwidthEstimator.cpp
            src/net/media/JitterBuffer.cpp
        )

        target_include_directories(audio_codec_smoke PRIVATE
            src
            ${FFMPEG_INCLUDE_DIR}
        )

        target_link_libraries(audio_codec_smoke PRIVATE
            Qt6::Core
            Qt6::Multimedia
            ${FFMPEG_AVCODEC_LIBRARY}
            ${FFMPEG_AVUTIL_LIBRARY}
            ${FFMPEG_SWRESAMPLE_LIBRARY}
            ${FFMPEG_SWSCALE_LIBRARY}
        )

        if(WIN32)
            target_link_libraries(audio_codec_smoke PRIVATE ws2_32 bcrypt)
        endif()

        add_test(NAME audio_codec_smoke COMMAND audio_codec_smoke)
    else()
        message(WARNING "Audio codec smoke target skipped: FFmpeg 7.1 headers/libs not found under ${FFMPEG_ROOT_HINT}.")
    endif()
endif()


