cmake_minimum_required(VERSION 3.20)
project(meeting_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick Network)
find_package(Protobuf REQUIRED)
qt_standard_project_setup()

qt_add_executable(meeting_client
    src/main.cpp
    src/app/MeetingController.cpp
    src/net/signaling/SignalProtocol.cpp
    src/net/signaling/SignalingClient.cpp
    src/net/signaling/Reconnector.cpp
    proto/pb/signaling.pb.cc
)

target_include_directories(meeting_client PRIVATE
    src
    proto/pb
)

target_link_libraries(meeting_client PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Network
    protobuf::libprotobuf
)

qt_add_qml_module(meeting_client
    URI MeetingApp
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/components/TitleBar.qml
)
