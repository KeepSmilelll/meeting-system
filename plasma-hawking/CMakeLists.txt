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
unset(FFMPEG_AVDEVICE_LIBRARY CACHE)
unset(FFMPEG_AVFORMAT_LIBRARY CACHE)
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
    find_library(FFMPEG_AVDEVICE_LIBRARY
        NAMES avdevice libavdevice
        HINTS "${FFMPEG_ROOT_HINT}/lib"
        NO_DEFAULT_PATH
    )
    find_library(FFMPEG_AVFORMAT_LIBRARY
        NAMES avformat libavformat
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

if(NOT (FFMPEG_INCLUDE_DIR AND FFMPEG_AVCODEC_LIBRARY AND FFMPEG_AVDEVICE_LIBRARY AND FFMPEG_AVFORMAT_LIBRARY AND FFMPEG_AVUTIL_LIBRARY AND FFMPEG_SWRESAMPLE_LIBRARY AND FFMPEG_SWSCALE_LIBRARY))
    message(FATAL_ERROR "meeting_client requires FFmpeg 7.1 headers/libs under ${FFMPEG_ROOT_HINT}")
endif()

set(FFMPEG_RUNTIME_DLLS)
if(WIN32 AND EXISTS "${FFMPEG_ROOT_HINT}/bin")
    foreach(ffmpeg_dll_name IN ITEMS avcodec-61.dll avdevice-61.dll avformat-61.dll avutil-59.dll swresample-5.dll swscale-8.dll)
        set(ffmpeg_dll_path "${FFMPEG_ROOT_HINT}/bin/${ffmpeg_dll_name}")
        if(EXISTS "${ffmpeg_dll_path}")
            list(APPEND FFMPEG_RUNTIME_DLLS "${ffmpeg_dll_path}")
        endif()
    endforeach()
endif()

function(copy_ffmpeg_runtime target_name)
    if(NOT WIN32)
        return()
    endif()
    foreach(ffmpeg_dll IN LISTS FFMPEG_RUNTIME_DLLS)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ffmpeg_dll}"
                $<TARGET_FILE_DIR:${target_name}>
        )
    endforeach()
endfunction()
qt_add_executable(meeting_client
    src/main.cpp
    src/app/AppStateMachine.cpp
    src/app/MeetingController.cpp
    src/app/MediaSessionManager.cpp
    src/app/ParticipantListModel.cpp
    src/app/RuntimeSmokeDriver.cpp
    src/app/UserManager.cpp
    src/app/EventBus.cpp
    src/security/CryptoUtils.cpp
    src/security/TokenManager.cpp
    src/security/SRTPContext.cpp
    src/av/capture/AudioCapture.cpp
    src/av/capture/CameraCapture.cpp
    src/av/capture/ScreenCapture.cpp
    src/av/codec/AudioEncoder.cpp
    src/av/codec/AudioDecoder.cpp
    src/av/codec/VideoDecoder.cpp
    src/av/codec/VideoEncoder.cpp
    src/av/process/AcousticEchoCanceller.cpp
    src/av/process/NoiseSuppressor.cpp
    src/av/process/AutoGainControl.cpp
    src/av/render/AudioPlayer.cpp
    src/av/render/NV12Shader.cpp
    src/av/render/VideoFrameStore.cpp
    src/av/render/VideoRenderer.cpp
    src/av/session/AudioCallSession.cpp
    src/av/session/ScreenShareSession.cpp
    src/av/session/ScreenShareSessionCamera.cpp
    src/av/session/ScreenShareSessionTransport.cpp
    src/av/session/CameraFrameRelay.cpp
    src/av/session/VideoSendFrameRingBuffer.cpp
    src/av/session/VideoSendPipeline.cpp
    src/av/session/VideoSendTelemetryPipeline.cpp
    src/av/session/VideoRecvPipeline.cpp
    src/av/session/VideoRecvConsumePipeline.cpp
    src/av/session/VideoRecvErrorPipeline.cpp
    src/av/session/VideoRecvIngressPipeline.cpp
    src/av/session/VideoRecvKeyFramePipeline.cpp
    src/av/session/VideoRecvFrameDispatchPipeline.cpp
    src/av/session/VideoRecvTelemetryPipeline.cpp
    src/av/session/VideoRecvRtcpPipeline.cpp
    src/av/session/VideoRtcpActionPipeline.cpp
    src/av/session/VideoRtcpFeedbackPipeline.cpp
    src/av/session/VideoRtcpFeedbackDispatchPipeline.cpp
    src/av/session/VideoSessionStateMachine.cpp
    src/av/session/VideoThreadLifecycleStateMachine.cpp
    src/av/session/VideoThreadRuntime.cpp
    src/av/session/VideoSessionControlActions.cpp
    src/av/session/VideoSendControlActions.cpp
    src/av/session/VideoSendSourcePipeline.cpp
    src/av/session/VideoSendLoopPipeline.cpp
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
    src/net/media/H264RtpPayload.cpp
    src/net/media/BandwidthEstimator.cpp
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
    ${FFMPEG_AVDEVICE_LIBRARY}
    ${FFMPEG_AVFORMAT_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
    ${FFMPEG_SWRESAMPLE_LIBRARY}
    ${FFMPEG_SWSCALE_LIBRARY}
)
copy_ffmpeg_runtime(meeting_client)

if(WIN32)
    target_link_libraries(meeting_client PRIVATE ws2_32 bcrypt opengl32)
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
        qml/components/VideoGrid.qml
        qml/components/VideoTile.qml
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
    src/av/capture/CameraCapture.cpp
    src/av/codec/VideoDecoder.cpp
    src/av/codec/VideoEncoder.cpp
    src/av/session/ScreenShareSession.cpp
    src/av/session/ScreenShareSessionCamera.cpp
    src/av/session/ScreenShareSessionTransport.cpp
    src/av/session/CameraFrameRelay.cpp
    src/av/session/VideoSendFrameRingBuffer.cpp
    src/av/session/VideoSendPipeline.cpp
    src/av/session/VideoSendTelemetryPipeline.cpp
    src/av/session/VideoRecvPipeline.cpp
    src/av/session/VideoRecvConsumePipeline.cpp
    src/av/session/VideoRecvErrorPipeline.cpp
    src/av/session/VideoRecvIngressPipeline.cpp
    src/av/session/VideoRecvKeyFramePipeline.cpp
    src/av/session/VideoRecvFrameDispatchPipeline.cpp
    src/av/session/VideoRecvTelemetryPipeline.cpp
    src/av/session/VideoRecvRtcpPipeline.cpp
    src/av/session/VideoRtcpActionPipeline.cpp
    src/av/session/VideoRtcpFeedbackPipeline.cpp
    src/av/session/VideoRtcpFeedbackDispatchPipeline.cpp
    src/av/session/VideoSessionStateMachine.cpp
    src/av/session/VideoThreadLifecycleStateMachine.cpp
    src/av/session/VideoThreadRuntime.cpp
    src/av/session/VideoSessionControlActions.cpp
    src/av/session/VideoSendControlActions.cpp
    src/av/session/VideoSendSourcePipeline.cpp
    src/av/session/VideoSendLoopPipeline.cpp
    src/net/media/RTPSender.cpp
    src/net/media/RTPReceiver.cpp
    src/net/media/H264RtpPayload.cpp
)

target_include_directories(test_screen_share_session PRIVATE
    src
    ${FFMPEG_INCLUDE_DIR}
)
target_link_libraries(test_screen_share_session PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Multimedia
    Qt6::Network
    ${FFMPEG_AVCODEC_LIBRARY}
    ${FFMPEG_AVDEVICE_LIBRARY}
    ${FFMPEG_AVFORMAT_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
    ${FFMPEG_SWSCALE_LIBRARY}
)
copy_ffmpeg_runtime(test_screen_share_session)

if(WIN32)
    target_link_libraries(test_screen_share_session PRIVATE ws2_32)
endif()

add_test(NAME test_screen_share_session COMMAND test_screen_share_session)

add_executable(test_screen_share_session_camera_smoke
    tests/test_screen_share_session_camera_smoke.cpp
    src/av/capture/CameraCapture.cpp
    src/av/capture/ScreenCapture.cpp
    src/av/codec/VideoDecoder.cpp
    src/av/codec/VideoEncoder.cpp
    src/av/session/ScreenShareSession.cpp
    src/av/session/ScreenShareSessionCamera.cpp
    src/av/session/ScreenShareSessionTransport.cpp
    src/av/session/CameraFrameRelay.cpp
    src/av/session/VideoSendFrameRingBuffer.cpp
    src/av/session/VideoSendPipeline.cpp
    src/av/session/VideoSendTelemetryPipeline.cpp
    src/av/session/VideoRecvPipeline.cpp
    src/av/session/VideoRecvConsumePipeline.cpp
    src/av/session/VideoRecvErrorPipeline.cpp
    src/av/session/VideoRecvIngressPipeline.cpp
    src/av/session/VideoRecvKeyFramePipeline.cpp
    src/av/session/VideoRecvFrameDispatchPipeline.cpp
    src/av/session/VideoRecvTelemetryPipeline.cpp
    src/av/session/VideoRecvRtcpPipeline.cpp
    src/av/session/VideoRtcpActionPipeline.cpp
    src/av/session/VideoRtcpFeedbackPipeline.cpp
    src/av/session/VideoRtcpFeedbackDispatchPipeline.cpp
    src/av/session/VideoSessionStateMachine.cpp
    src/av/session/VideoThreadLifecycleStateMachine.cpp
    src/av/session/VideoThreadRuntime.cpp
    src/av/session/VideoSessionControlActions.cpp
    src/av/session/VideoSendControlActions.cpp
    src/av/session/VideoSendSourcePipeline.cpp
    src/av/session/VideoSendLoopPipeline.cpp
    src/net/media/RTPSender.cpp
    src/net/media/RTPReceiver.cpp
    src/net/media/H264RtpPayload.cpp
)

target_include_directories(test_screen_share_session_camera_smoke PRIVATE
    src
    ${FFMPEG_INCLUDE_DIR}
)

target_link_libraries(test_screen_share_session_camera_smoke PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Multimedia
    Qt6::Network
    ${FFMPEG_AVCODEC_LIBRARY}
    ${FFMPEG_AVDEVICE_LIBRARY}
    ${FFMPEG_AVFORMAT_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
    ${FFMPEG_SWSCALE_LIBRARY}
)
copy_ffmpeg_runtime(test_screen_share_session_camera_smoke)

if(WIN32)
    target_link_libraries(test_screen_share_session_camera_smoke PRIVATE ws2_32)
endif()

add_test(NAME test_screen_share_session_camera_smoke COMMAND test_screen_share_session_camera_smoke)
set_tests_properties(test_screen_share_session_camera_smoke PROPERTIES
    SKIP_RETURN_CODE 77
)

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
add_executable(test_av_sync
    tests/test_av_sync.cpp
)

target_include_directories(test_av_sync PRIVATE
    src
)

add_test(NAME test_av_sync COMMAND test_av_sync)

add_executable(test_ring_buffer
    tests/test_ring_buffer.cpp
)

target_include_directories(test_ring_buffer PRIVATE
    src
)

add_test(NAME test_ring_buffer COMMAND test_ring_buffer)

add_executable(test_video_send_frame_ring_buffer
    tests/test_video_send_frame_ring_buffer.cpp
    src/av/session/VideoSendFrameRingBuffer.cpp
)

target_include_directories(test_video_send_frame_ring_buffer PRIVATE
    src
    ${FFMPEG_INCLUDE_DIR}
)

target_link_libraries(test_video_send_frame_ring_buffer PRIVATE
    Qt6::Core
    ${FFMPEG_AVCODEC_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
)

add_test(NAME test_video_send_frame_ring_buffer COMMAND test_video_send_frame_ring_buffer)

add_executable(test_jitter_buffer
    tests/test_jitter_buffer.cpp
    src/net/media/JitterBuffer.cpp
)

target_include_directories(test_jitter_buffer PRIVATE
    src
)

add_test(NAME test_jitter_buffer COMMAND test_jitter_buffer)

add_executable(test_h264_rtp_payload
    tests/test_h264_rtp_payload.cpp
    src/net/media/H264RtpPayload.cpp
)

target_include_directories(test_h264_rtp_payload PRIVATE
    src
)

add_test(NAME test_h264_rtp_payload COMMAND test_h264_rtp_payload)

add_executable(test_video_send_pipeline_avframe
    tests/test_video_send_pipeline_avframe.cpp
    src/av/codec/VideoEncoder.cpp
    src/av/session/VideoSendPipeline.cpp
    src/net/media/RTPSender.cpp
    src/net/media/H264RtpPayload.cpp
)

target_include_directories(test_video_send_pipeline_avframe PRIVATE
    src
    ${FFMPEG_INCLUDE_DIR}
)

target_link_libraries(test_video_send_pipeline_avframe PRIVATE
    ${FFMPEG_AVCODEC_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
)

add_test(NAME test_video_send_pipeline_avframe COMMAND test_video_send_pipeline_avframe)
set_tests_properties(test_video_send_pipeline_avframe PROPERTIES
    SKIP_RETURN_CODE 77
)

add_executable(test_video_recv_pipeline_multistream
    tests/test_video_recv_pipeline_multistream.cpp
    src/av/codec/VideoEncoder.cpp
    src/av/codec/VideoDecoder.cpp
    src/av/session/VideoSendPipeline.cpp
    src/av/session/VideoRecvPipeline.cpp
    src/net/media/RTPSender.cpp
    src/net/media/RTPReceiver.cpp
    src/net/media/H264RtpPayload.cpp
)

target_include_directories(test_video_recv_pipeline_multistream PRIVATE
    src
    ${FFMPEG_INCLUDE_DIR}
)

target_link_libraries(test_video_recv_pipeline_multistream PRIVATE
    ${FFMPEG_AVCODEC_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
    ${FFMPEG_AVDEVICE_LIBRARY}
    ${FFMPEG_AVFORMAT_LIBRARY}
    ${FFMPEG_SWSCALE_LIBRARY}
)
copy_ffmpeg_runtime(test_video_recv_pipeline_multistream)

if(WIN32)
    target_link_libraries(test_video_recv_pipeline_multistream PRIVATE ws2_32)
endif()

add_test(NAME test_video_recv_pipeline_multistream COMMAND test_video_recv_pipeline_multistream)
set_tests_properties(test_video_recv_pipeline_multistream PROPERTIES
    SKIP_RETURN_CODE 77
)

add_executable(test_security_utils
    tests/test_security_utils.cpp
    src/security/CryptoUtils.cpp
    src/security/TokenManager.cpp
    src/security/SRTPContext.cpp
    src/storage/DatabaseManager.cpp
    src/storage/SettingsRepository.cpp
)

target_include_directories(test_security_utils PRIVATE
    src
)

target_link_libraries(test_security_utils PRIVATE
    Qt6::Core
    Qt6::Sql
)

add_test(NAME test_security_utils COMMAND test_security_utils)

add_executable(test_event_bus
    tests/test_event_bus.cpp
    src/app/EventBus.cpp
)

target_include_directories(test_event_bus PRIVATE
    src
)

target_link_libraries(test_event_bus PRIVATE
    Qt6::Core
)

add_test(NAME test_event_bus COMMAND test_event_bus)

add_executable(test_meeting_controller_mute_all
    tests/test_meeting_controller_mute_all.cpp
    src/app/MeetingController.cpp
    src/app/MediaSessionManager.cpp
    src/app/ParticipantListModel.cpp
    src/app/UserManager.cpp
    src/security/CryptoUtils.cpp
    src/security/TokenManager.cpp
    src/av/capture/AudioCapture.cpp
    src/av/capture/CameraCapture.cpp
    src/av/capture/ScreenCapture.cpp
    src/av/codec/AudioEncoder.cpp
    src/av/codec/AudioDecoder.cpp
    src/av/codec/VideoDecoder.cpp
    src/av/codec/VideoEncoder.cpp
    src/av/process/AcousticEchoCanceller.cpp
    src/av/process/NoiseSuppressor.cpp
    src/av/process/AutoGainControl.cpp
    src/av/render/AudioPlayer.cpp
    src/av/render/VideoFrameStore.cpp
    src/av/session/AudioCallSession.cpp
    src/av/session/ScreenShareSession.cpp
    src/av/session/ScreenShareSessionCamera.cpp
    src/av/session/ScreenShareSessionTransport.cpp
    src/av/session/CameraFrameRelay.cpp
    src/av/session/VideoSendFrameRingBuffer.cpp
    src/av/session/VideoSendPipeline.cpp
    src/av/session/VideoSendTelemetryPipeline.cpp
    src/av/session/VideoRecvPipeline.cpp
    src/av/session/VideoRecvConsumePipeline.cpp
    src/av/session/VideoRecvErrorPipeline.cpp
    src/av/session/VideoRecvIngressPipeline.cpp
    src/av/session/VideoRecvKeyFramePipeline.cpp
    src/av/session/VideoRecvFrameDispatchPipeline.cpp
    src/av/session/VideoRecvTelemetryPipeline.cpp
    src/av/session/VideoRecvRtcpPipeline.cpp
    src/av/session/VideoRtcpActionPipeline.cpp
    src/av/session/VideoRtcpFeedbackPipeline.cpp
    src/av/session/VideoRtcpFeedbackDispatchPipeline.cpp
    src/av/session/VideoSessionStateMachine.cpp
    src/av/session/VideoThreadLifecycleStateMachine.cpp
    src/av/session/VideoThreadRuntime.cpp
    src/av/session/VideoSessionControlActions.cpp
    src/av/session/VideoSendControlActions.cpp
    src/av/session/VideoSendSourcePipeline.cpp
    src/av/session/VideoSendLoopPipeline.cpp
    src/storage/DatabaseManager.cpp
    src/storage/SettingsRepository.cpp
    src/storage/MeetingRepository.cpp
    src/storage/CallLogRepository.cpp
    src/net/signaling/SignalProtocol.cpp
    src/net/signaling/SignalingClient.cpp
    src/net/signaling/Reconnector.cpp
    src/net/media/RTPSender.cpp
    src/net/media/RTPReceiver.cpp
    src/net/media/H264RtpPayload.cpp
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
    ${FFMPEG_AVDEVICE_LIBRARY}
    ${FFMPEG_AVFORMAT_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
    ${FFMPEG_SWRESAMPLE_LIBRARY}
    ${FFMPEG_SWSCALE_LIBRARY}
)
copy_ffmpeg_runtime(test_meeting_controller_mute_all)

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
    src/security/CryptoUtils.cpp
    src/security/TokenManager.cpp
    src/av/capture/AudioCapture.cpp
    src/av/capture/CameraCapture.cpp
    src/av/capture/ScreenCapture.cpp
    src/av/codec/AudioEncoder.cpp
    src/av/codec/AudioDecoder.cpp
    src/av/codec/VideoDecoder.cpp
    src/av/codec/VideoEncoder.cpp
    src/av/process/AcousticEchoCanceller.cpp
    src/av/process/NoiseSuppressor.cpp
    src/av/process/AutoGainControl.cpp
    src/av/render/AudioPlayer.cpp
    src/av/render/VideoFrameStore.cpp
    src/av/session/AudioCallSession.cpp
    src/av/session/ScreenShareSession.cpp
    src/av/session/ScreenShareSessionCamera.cpp
    src/av/session/ScreenShareSessionTransport.cpp
    src/av/session/CameraFrameRelay.cpp
    src/av/session/VideoSendFrameRingBuffer.cpp
    src/av/session/VideoSendPipeline.cpp
    src/av/session/VideoSendTelemetryPipeline.cpp
    src/av/session/VideoRecvPipeline.cpp
    src/av/session/VideoRecvConsumePipeline.cpp
    src/av/session/VideoRecvErrorPipeline.cpp
    src/av/session/VideoRecvIngressPipeline.cpp
    src/av/session/VideoRecvKeyFramePipeline.cpp
    src/av/session/VideoRecvFrameDispatchPipeline.cpp
    src/av/session/VideoRecvTelemetryPipeline.cpp
    src/av/session/VideoRecvRtcpPipeline.cpp
    src/av/session/VideoRtcpActionPipeline.cpp
    src/av/session/VideoRtcpFeedbackPipeline.cpp
    src/av/session/VideoRtcpFeedbackDispatchPipeline.cpp
    src/av/session/VideoSessionStateMachine.cpp
    src/av/session/VideoThreadLifecycleStateMachine.cpp
    src/av/session/VideoThreadRuntime.cpp
    src/av/session/VideoSessionControlActions.cpp
    src/av/session/VideoSendControlActions.cpp
    src/av/session/VideoSendSourcePipeline.cpp
    src/av/session/VideoSendLoopPipeline.cpp
    src/storage/DatabaseManager.cpp
    src/storage/SettingsRepository.cpp
    src/storage/MeetingRepository.cpp
    src/storage/CallLogRepository.cpp
    src/net/signaling/SignalProtocol.cpp
    src/net/signaling/SignalingClient.cpp
    src/net/signaling/Reconnector.cpp
    src/net/media/RTPSender.cpp
    src/net/media/RTPReceiver.cpp
    src/net/media/H264RtpPayload.cpp
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
    ${FFMPEG_AVDEVICE_LIBRARY}
    ${FFMPEG_AVFORMAT_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
    ${FFMPEG_SWRESAMPLE_LIBRARY}
    ${FFMPEG_SWSCALE_LIBRARY}
)
copy_ffmpeg_runtime(test_meeting_runtime_smoke)

if(WIN32)
    target_link_libraries(test_meeting_runtime_smoke PRIVATE ws2_32 bcrypt)
endif()

add_test(NAME test_meeting_runtime_smoke COMMAND test_meeting_runtime_smoke)

add_executable(test_camera_capture_smoke
    tests/test_camera_capture_smoke.cpp
    src/av/capture/CameraCapture.cpp
)

target_include_directories(test_camera_capture_smoke PRIVATE
    src
    ${FFMPEG_INCLUDE_DIR}
)

target_link_libraries(test_camera_capture_smoke PRIVATE
    Qt6::Core
    Qt6::Multimedia
    ${FFMPEG_AVCODEC_LIBRARY}
    ${FFMPEG_AVDEVICE_LIBRARY}
    ${FFMPEG_AVFORMAT_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
    ${FFMPEG_SWSCALE_LIBRARY}
)
copy_ffmpeg_runtime(test_camera_capture_smoke)

add_test(NAME test_camera_capture_smoke COMMAND test_camera_capture_smoke)
set_tests_properties(test_camera_capture_smoke PROPERTIES
    SKIP_RETURN_CODE 77
)

add_executable(test_audio_process
    tests/test_audio_process.cpp
    src/av/process/AcousticEchoCanceller.cpp
    src/av/process/NoiseSuppressor.cpp
    src/av/process/AutoGainControl.cpp
)

target_include_directories(test_audio_process PRIVATE
    src
)

target_link_libraries(test_audio_process PRIVATE
    Qt6::Core
)

add_test(NAME test_audio_process COMMAND test_audio_process)

add_executable(test_audio_process_quality
    tests/test_audio_process_quality.cpp
    src/av/process/AcousticEchoCanceller.cpp
    src/av/process/NoiseSuppressor.cpp
    src/av/process/AutoGainControl.cpp
)

target_include_directories(test_audio_process_quality PRIVATE
    src
)

target_link_libraries(test_audio_process_quality PRIVATE
    Qt6::Core
)

add_test(NAME test_audio_process_quality COMMAND test_audio_process_quality)

add_executable(test_meeting_client_process_smoke
    tests/test_meeting_client_process_smoke.cpp
    src/av/capture/CameraCapture.cpp
    src/av/codec/VideoEncoder.cpp
)

target_include_directories(test_meeting_client_process_smoke PRIVATE
    src
    ${FFMPEG_INCLUDE_DIR}
)

target_link_libraries(test_meeting_client_process_smoke PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Multimedia
    ${FFMPEG_AVCODEC_LIBRARY}
    ${FFMPEG_AVDEVICE_LIBRARY}
    ${FFMPEG_AVFORMAT_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
    ${FFMPEG_SWSCALE_LIBRARY}
)
copy_ffmpeg_runtime(test_meeting_client_process_smoke)

add_dependencies(test_meeting_client_process_smoke meeting_client)

add_test(NAME test_meeting_client_process_smoke COMMAND test_meeting_client_process_smoke)
add_test(NAME test_meeting_client_real_audio_smoke COMMAND test_meeting_client_process_smoke)
set_tests_properties(test_meeting_client_real_audio_smoke PROPERTIES
    ENVIRONMENT "MEETING_PROCESS_SMOKE_SYNTHETIC_AUDIO=0;MEETING_PROCESS_SMOKE_REQUIRE_AUDIO=1"
    SKIP_RETURN_CODE 77
)

add_test(NAME test_meeting_runtime_real_audio_smoke COMMAND test_meeting_runtime_smoke)
set_tests_properties(test_meeting_runtime_real_audio_smoke PROPERTIES
    ENVIRONMENT "MEETING_RUNTIME_SMOKE_SYNTHETIC_AUDIO=0"
    SKIP_RETURN_CODE 77
)

if(MEETING_BUILD_AUDIO_CODEC_SMOKE)
    if(FFMPEG_INCLUDE_DIR AND FFMPEG_AVCODEC_LIBRARY AND FFMPEG_AVUTIL_LIBRARY AND FFMPEG_SWRESAMPLE_LIBRARY AND FFMPEG_SWSCALE_LIBRARY)
        add_executable(audio_codec_smoke
            tests/AudioCodecSmoke.cpp
            src/av/capture/AudioCapture.cpp
            src/av/codec/AudioEncoder.cpp
            src/av/codec/AudioDecoder.cpp
            src/av/process/AcousticEchoCanceller.cpp
            src/av/process/NoiseSuppressor.cpp
            src/av/process/AutoGainControl.cpp
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
        copy_ffmpeg_runtime(audio_codec_smoke)

        if(WIN32)
            target_link_libraries(audio_codec_smoke PRIVATE ws2_32 bcrypt)
        endif()

        add_test(NAME audio_codec_smoke COMMAND audio_codec_smoke)
    else()
        message(WARNING "Audio codec smoke target skipped: FFmpeg 7.1 headers/libs not found under ${FFMPEG_ROOT_HINT}.")
    endif()
endif()









