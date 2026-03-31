#include "server/MediaIngress.h"
#include "server/RpcProtocol.h"
#include "server/RpcServer.h"
#include "server/RpcService.h"

#include "proto/pb/sfu_rpc.pb.h"
#include "room/Subscriber.h"
#include "room/RoomManager.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

template <typename MessageT>
bool SerializeMessage(const MessageT& message, std::vector<uint8_t>* out) {
    if (out == nullptr) {
        return false;
    }

    std::string bytes;
    if (!message.SerializeToString(&bytes)) {
        return false;
    }

    out->assign(bytes.begin(), bytes.end());
    return true;
}

template <typename MessageT>
bool ParseMessage(const std::vector<uint8_t>& data, MessageT* message) {
    return message != nullptr && message->ParseFromArray(data.data(), static_cast<int>(data.size()));
}

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

bool InitSockets() {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

void CleanupSockets() {
    WSACleanup();
}

void CloseSocket(SocketHandle socketHandle) {
    if (socketHandle != kInvalidSocket) {
        closesocket(socketHandle);
    }
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

bool InitSockets() {
    return true;
}

void CleanupSockets() {}

void CloseSocket(SocketHandle socketHandle) {
    if (socketHandle != kInvalidSocket) {
        close(socketHandle);
    }
}
#endif

bool ReadExact(SocketHandle socketHandle, uint8_t* buffer, std::size_t length) {
    std::size_t offset = 0;
    while (offset < length) {
        const auto received = recv(socketHandle, reinterpret_cast<char*>(buffer + offset),
                                   static_cast<int>(length - offset), 0);
        if (received <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(received);
    }
    return true;
}

bool WriteExact(SocketHandle socketHandle, const uint8_t* buffer, std::size_t length) {
    std::size_t offset = 0;
    while (offset < length) {
        const auto sent = send(socketHandle, reinterpret_cast<const char*>(buffer + offset),
                               static_cast<int>(length - offset), 0);
        if (sent <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

bool SendFrameAndReceiveResponse(SocketHandle socketHandle,
                                 const sfu::RpcFrame& request,
                                 sfu::RpcFrame* response) {
    std::vector<uint8_t> requestBytes;
    if (!sfu::EncodeRpcFrame(request, &requestBytes)) {
        return false;
    }

    if (!WriteExact(socketHandle, requestBytes.data(), requestBytes.size())) {
        return false;
    }

    std::vector<uint8_t> header(sfu::kRpcHeaderSize);
    if (!ReadExact(socketHandle, header.data(), header.size())) {
        return false;
    }

    const uint32_t payloadSize = (static_cast<uint32_t>(header[10]) << 24U) |
                                 (static_cast<uint32_t>(header[11]) << 16U) |
                                 (static_cast<uint32_t>(header[12]) << 8U) |
                                 static_cast<uint32_t>(header[13]);
    std::vector<uint8_t> body(payloadSize);
    if (payloadSize > 0 && !ReadExact(socketHandle, body.data(), body.size())) {
        return false;
    }

    std::vector<uint8_t> responseBytes;
    responseBytes.reserve(header.size() + body.size());
    responseBytes.insert(responseBytes.end(), header.begin(), header.end());
    responseBytes.insert(responseBytes.end(), body.begin(), body.end());
    return sfu::DecodeRpcFrame(responseBytes.data(), responseBytes.size(), response);
}

bool ConnectLoopback(uint16_t port, SocketHandle* outSocket) {
    if (outSocket == nullptr) {
        return false;
    }

    SocketHandle socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == kInvalidSocket) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
#else
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        CloseSocket(socketHandle);
        return false;
    }
#endif

    for (int attempt = 0; attempt < 50; ++attempt) {
        if (connect(socketHandle, reinterpret_cast<sockaddr*>(&addr), static_cast<int>(sizeof(addr))) == 0) {
            *outSocket = socketHandle;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CloseSocket(socketHandle);
    return false;
}

bool CreateBoundUdpSocket(SocketHandle* outSocket, uint16_t* outPort) {
    if (outSocket == nullptr || outPort == nullptr) {
        return false;
    }

    SocketHandle socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == kInvalidSocket) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
#else
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        CloseSocket(socketHandle);
        return false;
    }
#endif

    if (bind(socketHandle, reinterpret_cast<sockaddr*>(&addr), static_cast<int>(sizeof(addr))) != 0) {
        CloseSocket(socketHandle);
        return false;
    }

    sockaddr_in localAddr{};
#ifdef _WIN32
    int localAddrLen = static_cast<int>(sizeof(localAddr));
#else
    socklen_t localAddrLen = static_cast<socklen_t>(sizeof(localAddr));
#endif
    if (getsockname(socketHandle, reinterpret_cast<sockaddr*>(&localAddr), &localAddrLen) != 0) {
        CloseSocket(socketHandle);
        return false;
    }

    *outPort = ntohs(localAddr.sin_port);
    *outSocket = socketHandle;
    return true;
}

bool SendUdpPacket(SocketHandle socketHandle, uint16_t port, const uint8_t* data, std::size_t len) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
#else
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        return false;
    }
#endif

    const auto sent = sendto(socketHandle,
                             reinterpret_cast<const char*>(data),
                             static_cast<int>(len),
                             0,
                             reinterpret_cast<sockaddr*>(&addr),
                             static_cast<int>(sizeof(addr)));
    return sent == static_cast<int>(len);
}

bool ReceiveUdpPacket(SocketHandle socketHandle, std::vector<uint8_t>* out, int timeoutMs = 2000) {
    if (out == nullptr) {
        return false;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socketHandle, &readSet);

    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

#ifdef _WIN32
    const int rc = select(0, &readSet, nullptr, nullptr, &tv);
#else
    const int rc = select(socketHandle + 1, &readSet, nullptr, nullptr, &tv);
#endif
    if (rc <= 0 || !FD_ISSET(socketHandle, &readSet)) {
        return false;
    }

    std::vector<uint8_t> buffer(2048);
    sockaddr_in from{};
#ifdef _WIN32
    int fromLen = static_cast<int>(sizeof(from));
#else
    socklen_t fromLen = static_cast<socklen_t>(sizeof(from));
#endif
    const auto received = recvfrom(socketHandle,
                                   reinterpret_cast<char*>(buffer.data()),
                                   static_cast<int>(buffer.size()),
                                   0,
                                   reinterpret_cast<sockaddr*>(&from),
                                   &fromLen);
    if (received <= 0) {
        return false;
    }

    buffer.resize(static_cast<std::size_t>(received));
    *out = std::move(buffer);
    return true;
}

bool TestFrameRoundTrip() {
    sfu::RpcFrame request;
    request.method = sfu::RpcMethod::kAddPublisher;
    request.kind = sfu::RpcFrameKind::kResponse;
    request.status = 7;
    request.payload = {1, 2, 3, 4, 5};

    std::vector<uint8_t> encoded;
    if (!sfu::EncodeRpcFrame(request, &encoded)) {
        std::cerr << "EncodeRpcFrame failed\n";
        return false;
    }

    sfu::RpcFrame decoded;
    if (!sfu::DecodeRpcFrame(encoded.data(), encoded.size(), &decoded)) {
        std::cerr << "DecodeRpcFrame failed\n";
        return false;
    }

    return decoded.method == request.method &&
           decoded.kind == request.kind &&
           decoded.status == request.status &&
           decoded.payload == request.payload;
}

bool TestProtobufWireCompatibility() {
    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-42");
    createReq.set_max_publishers(3);

    std::string createBytes;
    if (!createReq.SerializeToString(&createBytes)) {
        std::cerr << "CreateRoomReq serialization failed\n";
        return false;
    }

    const std::vector<uint8_t> expectedCreate{
        0x0A, 0x07, 'r', 'o', 'o', 'm', '-', '4', '2',
        0x10, 0x03,
    };
    if (std::vector<uint8_t>(createBytes.begin(), createBytes.end()) != expectedCreate) {
        std::cerr << "CreateRoomReq protobuf bytes mismatch\n";
        return false;
    }

    sfu_rpc::AddPublisherRsp addRsp;
    if (!addRsp.ParseFromArray("\x08\x01\x10\xB9\x60", 5)) {
        std::cerr << "AddPublisherRsp parse failed\n";
        return false;
    }
    if (!addRsp.success() || addRsp.udp_port() != 12345) {
        std::cerr << "AddPublisherRsp protobuf parse mismatch\n";
        return false;
    }

    return true;
}

bool TestServiceLifecycle() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);
    service.SetAdvertisedAddress("127.0.0.1:4567");

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-100");
    createReq.set_max_publishers(2);

    std::vector<uint8_t> payload;
    if (!SerializeMessage(createReq, &payload)) {
        std::cerr << "Serialize create request failed\n";
        return false;
    }

    std::vector<uint8_t> responseBytes;
    if (!service.Dispatch(sfu::RpcMethod::kCreateRoom, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch create failed\n";
        return false;
    }

    sfu_rpc::CreateRoomRsp createRsp;
    if (!ParseMessage(responseBytes, &createRsp) || !createRsp.success() || createRsp.sfu_address() != "127.0.0.1:4567") {
        std::cerr << "Create response mismatch\n";
        return false;
    }
    if (!roomManager->HasRoom("room-100")) {
        std::cerr << "Room should exist after create\n";
        return false;
    }

    sfu_rpc::AddPublisherReq addReq;
    addReq.set_meeting_id("room-100");
    addReq.set_user_id("alice");
    addReq.set_audio_ssrc(11);
    addReq.set_video_ssrc(22);
    if (!SerializeMessage(addReq, &payload)) {
        std::cerr << "Serialize add request failed\n";
        return false;
    }

    if (!service.Dispatch(sfu::RpcMethod::kAddPublisher, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch add failed\n";
        return false;
    }

    sfu_rpc::AddPublisherRsp addRsp;
    if (!ParseMessage(responseBytes, &addRsp) || !addRsp.success()) {
        std::cerr << "Add response mismatch\n";
        return false;
    }
    if (addRsp.udp_port() == 0 || addRsp.udp_port() != service.MediaPort()) {
        std::cerr << "AddPublisher should return the live media ingress port\n";
        return false;
    }

    const auto room = roomManager->GetRoomShared("room-100");
    if (!room || room->GetPublisher("alice") == nullptr || room->GetPublisher("alice")->AudioSsrc() != 11) {
        std::cerr << "Publisher should be present in room\n";
        return false;
    }

    sfu_rpc::RemovePublisherReq removeReq;
    removeReq.set_meeting_id("room-100");
    removeReq.set_user_id("alice");
    if (!SerializeMessage(removeReq, &payload)) {
        std::cerr << "Serialize remove request failed\n";
        return false;
    }

    if (!service.Dispatch(sfu::RpcMethod::kRemovePublisher, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch remove failed\n";
        return false;
    }

    sfu_rpc::RemovePublisherRsp removeRsp;
    if (!ParseMessage(responseBytes, &removeRsp) || !removeRsp.success()) {
        std::cerr << "Remove response mismatch\n";
        return false;
    }
    if (room->GetPublisher("alice") != nullptr) {
        std::cerr << "Publisher should be removed from room\n";
        return false;
    }

    sfu_rpc::DestroyRoomReq destroyReq;
    destroyReq.set_meeting_id("room-100");
    if (!SerializeMessage(destroyReq, &payload)) {
        std::cerr << "Serialize destroy request failed\n";
        return false;
    }

    if (!service.Dispatch(sfu::RpcMethod::kDestroyRoom, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch destroy failed\n";
        return false;
    }

    sfu_rpc::DestroyRoomRsp destroyRsp;
    if (!ParseMessage(responseBytes, &destroyRsp) || !destroyRsp.success() || roomManager->HasRoom("room-100")) {
        std::cerr << "Destroy response mismatch\n";
        return false;
    }

    return true;
}

bool TestMediaIngressRouting() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);
    service.SetAdvertisedAddress("127.0.0.1:4567");

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-udp");
    createReq.set_max_publishers(2);
    std::vector<uint8_t> payload;
    if (!SerializeMessage(createReq, &payload)) {
        std::cerr << "Serialize create request failed\n";
        return false;
    }

    std::vector<uint8_t> responseBytes;
    if (!service.Dispatch(sfu::RpcMethod::kCreateRoom, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch create failed\n";
        return false;
    }

    uint16_t subscriberPort = 0;
    SocketHandle subscriberSocket = kInvalidSocket;
    if (!CreateBoundUdpSocket(&subscriberSocket, &subscriberPort)) {
        std::cerr << "CreateBoundUdpSocket failed\n";
        return false;
    }

    auto room = roomManager->GetRoomShared("room-udp");
    if (!room) {
        std::cerr << "Room should exist before routing test\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    auto subscriber = std::make_shared<sfu::Subscriber>("bob", "127.0.0.1:" + std::to_string(subscriberPort), 0x22222222U, 0);
    if (!room->AddSubscriber(subscriber)) {
        std::cerr << "AddSubscriber failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    sfu_rpc::AddPublisherReq addReq;
    addReq.set_meeting_id("room-udp");
    addReq.set_user_id("alice");
    addReq.set_audio_ssrc(0x11111111U);
    addReq.set_video_ssrc(0);
    if (!SerializeMessage(addReq, &payload)) {
        std::cerr << "Serialize add request failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    if (!service.Dispatch(sfu::RpcMethod::kAddPublisher, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch add failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    sfu_rpc::AddPublisherRsp addRsp;
    if (!ParseMessage(responseBytes, &addRsp) || !addRsp.success() || addRsp.udp_port() == 0) {
        std::cerr << "AddPublisher should return a live UDP port\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    SocketHandle sendSocket = kInvalidSocket;
    uint16_t senderPort = 0;
    if (!CreateBoundUdpSocket(&sendSocket, &senderPort)) {
        std::cerr << "CreateBoundUdpSocket for sender failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> rtpPacket = {
        0x80, 0x60,
        0x00, 0x2A,
        0x00, 0x00, 0x00, 0x64,
        0x11, 0x11, 0x11, 0x11,
        0xAB, 0xCD, 0xEF
    };

    if (!SendUdpPacket(sendSocket, addRsp.udp_port(), rtpPacket.data(), rtpPacket.size())) {
        std::cerr << "SendUdpPacket failed\n";
        CloseSocket(sendSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> forwardedPacket;
    if (!ReceiveUdpPacket(subscriberSocket, &forwardedPacket)) {
        std::cerr << "Expected forwarded RTP packet on subscriber endpoint\n";
        CloseSocket(sendSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    if (forwardedPacket.size() != rtpPacket.size() ||
        forwardedPacket[8] != 0x22 || forwardedPacket[9] != 0x22 ||
        forwardedPacket[10] != 0x22 || forwardedPacket[11] != 0x22) {
        std::cerr << "Forwarded RTP packet SSRC rewrite mismatch\n";
        CloseSocket(sendSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    sfu::MediaIngress::PacketObservation observation;
    if (!service.mediaIngress()->LastObservation(&observation) ||
        observation.meetingId != "room-udp" ||
        observation.userId != "alice" ||
        observation.ssrc != 0x11111111U ||
        observation.sequence != 42) {
        std::cerr << "MediaIngress did not resolve the publisher correctly\n";
        CloseSocket(sendSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    CloseSocket(sendSocket);
    CloseSocket(subscriberSocket);
    return service.mediaIngress()->PacketCount() > 0;
}

bool TestServerLoopback() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    auto service = std::make_shared<sfu::RpcService>(roomManager);
    sfu::RpcServer server(0, service);
    if (!server.Start()) {
        std::cerr << "RpcServer Start failed\n";
        return false;
    }

    const uint16_t port = server.Port();
    if (port == 0) {
        std::cerr << "RpcServer should expose a port\n";
        server.Stop();
        return false;
    }

    SocketHandle socketHandle = kInvalidSocket;
    if (!ConnectLoopback(port, &socketHandle)) {
        std::cerr << "ConnectLoopback failed\n";
        server.Stop();
        return false;
    }

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("rpc-room");
    createReq.set_max_publishers(1);
    std::vector<uint8_t> payload;
    if (!SerializeMessage(createReq, &payload)) {
        std::cerr << "Serialize create request failed\n";
        CloseSocket(socketHandle);
        server.Stop();
        return false;
    }

    sfu::RpcFrame request;
    request.method = sfu::RpcMethod::kCreateRoom;
    request.kind = sfu::RpcFrameKind::kRequest;
    request.payload = std::move(payload);

    sfu::RpcFrame response;
    const bool ok = SendFrameAndReceiveResponse(socketHandle, request, &response);
    CloseSocket(socketHandle);
    server.Stop();
    if (!ok) {
        std::cerr << "Loopback request failed\n";
        return false;
    }

    if (response.method != sfu::RpcMethod::kCreateRoom ||
        response.kind != sfu::RpcFrameKind::kResponse ||
        response.status != 0) {
        std::cerr << "Loopback response header mismatch\n";
        return false;
    }

    sfu_rpc::CreateRoomRsp rsp;
    if (!ParseMessage(response.payload, &rsp) || !rsp.success() || rsp.sfu_address() != service->AdvertisedAddress()) {
        std::cerr << "Loopback response payload mismatch\n";
        return false;
    }

    return roomManager->HasRoom("rpc-room");
}

} // namespace

int main() {
    if (!InitSockets()) {
        std::cerr << "socket init failed\n";
        return 1;
    }

    const bool ok = TestFrameRoundTrip() &&
                    TestProtobufWireCompatibility() &&
                    TestServiceLifecycle() &&
                    TestMediaIngressRouting() &&
                    TestServerLoopback();
    CleanupSockets();

    if (!ok) {
        return 1;
    }

    std::cout << "sfu_rpc_server_tests passed\n";
    return 0;
}

