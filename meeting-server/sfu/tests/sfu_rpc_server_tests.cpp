#include "server/MediaIngress.h"
#include "server/RpcProtocol.h"
#include "server/RpcServer.h"
#include "server/RpcService.h"
#include "server/DtlsTransport.h"
#include "server/SrtpSession.h"

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

std::vector<uint8_t> BuildStunBindingRequest(const std::string& username) {
    if (username.empty() || username.size() > 255U) {
        return {};
    }

    const std::size_t attributeLength = username.size();
    const std::size_t paddedAttributeLength = (attributeLength + 3U) & ~std::size_t{3U};
    const std::size_t messageLength = 4U + paddedAttributeLength;
    std::vector<uint8_t> packet(20U + messageLength, 0U);
    packet[0] = 0x00U;
    packet[1] = 0x01U;
    packet[2] = static_cast<uint8_t>((messageLength >> 8U) & 0xFFU);
    packet[3] = static_cast<uint8_t>(messageLength & 0xFFU);
    packet[4] = 0x21U;
    packet[5] = 0x12U;
    packet[6] = 0xA4U;
    packet[7] = 0x42U;
    for (std::size_t i = 8U; i < 20U; ++i) {
        packet[i] = static_cast<uint8_t>(0xA0U + (i - 8U));
    }

    packet[20] = 0x00U;
    packet[21] = 0x06U;
    packet[22] = static_cast<uint8_t>((attributeLength >> 8U) & 0xFFU);
    packet[23] = static_cast<uint8_t>(attributeLength & 0xFFU);
    std::memcpy(packet.data() + 24U, username.data(), username.size());
    return packet;
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
    if (!addRsp.ParseFromArray("\x08\x01", 2)) {
        std::cerr << "AddPublisherRsp parse failed\n";
        return false;
    }
    if (!addRsp.success()) {
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

    addReq.set_audio_ssrc(33);
    addReq.set_video_ssrc(44);
    if (!SerializeMessage(addReq, &payload)) {
        std::cerr << "Serialize second add request failed\n";
        return false;
    }

    if (!service.Dispatch(sfu::RpcMethod::kAddPublisher, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch second add failed\n";
        return false;
    }

    if (!ParseMessage(responseBytes, &addRsp) || !addRsp.success()) {
        std::cerr << "Second add response mismatch\n";
        return false;
    }
    if (!room || room->GetPublisher("alice") == nullptr || room->GetPublisher("alice")->AudioSsrc() != 33) {
        std::cerr << "Publisher should be updated on repeated add\n";
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

bool TestAddPublisherRefreshesExistingBinding() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);
    service.SetAdvertisedAddress("127.0.0.1:4567");

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-refresh");
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

    sfu_rpc::AddPublisherReq addReq;
    addReq.set_meeting_id("room-refresh");
    addReq.set_user_id("alice");
    addReq.set_audio_ssrc(0x11111111U);
    if (!SerializeMessage(addReq, &payload) ||
        !service.Dispatch(sfu::RpcMethod::kAddPublisher, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch initial add failed\n";
        return false;
    }

    addReq.set_audio_ssrc(0x33333333U);
    if (!SerializeMessage(addReq, &payload) ||
        !service.Dispatch(sfu::RpcMethod::kAddPublisher, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch refreshed add failed\n";
        return false;
    }

    const auto room = roomManager->GetRoomShared("room-refresh");
    if (!room || room->PublisherCount() != 1) {
        std::cerr << "Expected one publisher after refresh\n";
        return false;
    }

    const auto publisher = room->GetPublisher("alice");
    if (!publisher || publisher->AudioSsrc() != 0x33333333U) {
        std::cerr << "Publisher SSRC was not refreshed\n";
        return false;
    }

    sfu::RoomManager::PublisherLocation location;
    if (service.mediaIngress()->ResolvePublisher(0x11111111U, &location)) {
        std::cerr << "Old SSRC should no longer resolve\n";
        return false;
    }
    if (!service.mediaIngress()->ResolvePublisher(0x33333333U, &location) || location.publisher.userId != "alice") {
        std::cerr << "New SSRC should resolve to alice\n";
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
    if (!ParseMessage(responseBytes, &addRsp) || !addRsp.success() || service.MediaPort() == 0) {
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

    if (!SendUdpPacket(sendSocket, service.MediaPort(), rtpPacket.data(), rtpPacket.size())) {
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

bool TestMediaIngressRetransmitsOnNack() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-nack");
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

    auto room = roomManager->GetRoomShared("room-nack");
    if (!room) {
        std::cerr << "Room should exist before nack test\n";
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
    addReq.set_meeting_id("room-nack");
    addReq.set_user_id("alice");
    addReq.set_audio_ssrc(0x11111111U);
    addReq.set_video_ssrc(0);
    if (!SerializeMessage(addReq, &payload) ||
        !service.Dispatch(sfu::RpcMethod::kAddPublisher, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch add failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    sfu_rpc::AddPublisherRsp addRsp;
    if (!ParseMessage(responseBytes, &addRsp) || !addRsp.success() || service.MediaPort() == 0) {
        std::cerr << "AddPublisher should return a live UDP port\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    SocketHandle senderSocket = kInvalidSocket;
    uint16_t senderPort = 0;
    if (!CreateBoundUdpSocket(&senderSocket, &senderPort)) {
        std::cerr << "CreateBoundUdpSocket for sender failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    const std::vector<uint8_t> rtpPacket = {
        0x80, 0x60,
        0x00, 0x2A,
        0x00, 0x00, 0x00, 0x64,
        0x11, 0x11, 0x11, 0x11,
        0xAB, 0xCD, 0xEF
    };
    if (!SendUdpPacket(senderSocket, service.MediaPort(), rtpPacket.data(), rtpPacket.size())) {
        std::cerr << "SendUdpPacket RTP failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> firstForwardedPacket;
    if (!ReceiveUdpPacket(subscriberSocket, &firstForwardedPacket)) {
        std::cerr << "Expected initial forwarded RTP packet\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    const std::vector<uint8_t> nackPacket = {
        0x81, 0xCD, 0x00, 0x03,
        0x22, 0x22, 0x22, 0x22,
        0x22, 0x22, 0x22, 0x22,
        0x00, 0x2A, 0x00, 0x00,
    };
    if (!SendUdpPacket(subscriberSocket, service.MediaPort(), nackPacket.data(), nackPacket.size())) {
        std::cerr << "SendUdpPacket NACK failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> retransmitPacket;
    if (!ReceiveUdpPacket(subscriberSocket, &retransmitPacket, 1000)) {
        std::cerr << "Expected retransmitted RTP packet after NACK\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    const bool validRetransmit = retransmitPacket.size() == rtpPacket.size() &&
                                 retransmitPacket[2] == 0x00 &&
                                 retransmitPacket[3] == 0x2A &&
                                 retransmitPacket[8] == 0x22 &&
                                 retransmitPacket[9] == 0x22 &&
                                 retransmitPacket[10] == 0x22 &&
                                 retransmitPacket[11] == 0x22;
    if (!validRetransmit) {
        std::cerr << "Retransmit packet mismatch after NACK\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    CloseSocket(senderSocket);
    CloseSocket(subscriberSocket);
    return true;
}

bool TestMediaIngressForwardsPliToPublisher() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-pli");
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

    auto room = roomManager->GetRoomShared("room-pli");
    if (!room) {
        std::cerr << "Room should exist before pli test\n";
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
    addReq.set_meeting_id("room-pli");
    addReq.set_user_id("alice");
    addReq.set_audio_ssrc(0x11111111U);
    if (!SerializeMessage(addReq, &payload) ||
        !service.Dispatch(sfu::RpcMethod::kAddPublisher, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch add failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    sfu_rpc::AddPublisherRsp addRsp;
    if (!ParseMessage(responseBytes, &addRsp) || !addRsp.success() || service.MediaPort() == 0) {
        std::cerr << "AddPublisher should return a live UDP port\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    SocketHandle senderSocket = kInvalidSocket;
    uint16_t senderPort = 0;
    if (!CreateBoundUdpSocket(&senderSocket, &senderPort)) {
        std::cerr << "CreateBoundUdpSocket for sender failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    const std::vector<uint8_t> rtpPacket = {
        0x80, 0x60,
        0x00, 0x2A,
        0x00, 0x00, 0x00, 0x64,
        0x11, 0x11, 0x11, 0x11,
        0xAB, 0xCD, 0xEF
    };
    if (!SendUdpPacket(senderSocket, service.MediaPort(), rtpPacket.data(), rtpPacket.size())) {
        std::cerr << "SendUdpPacket RTP failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> forwardedPacket;
    if (!ReceiveUdpPacket(subscriberSocket, &forwardedPacket)) {
        std::cerr << "Expected initial forwarded RTP packet\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    const std::vector<uint8_t> pliPacket = {
        0x81, 0xCE, 0x00, 0x02,
        0x22, 0x22, 0x22, 0x22,
        0x22, 0x22, 0x22, 0x22,
    };
    if (!SendUdpPacket(subscriberSocket, service.MediaPort(), pliPacket.data(), pliPacket.size())) {
        std::cerr << "SendUdpPacket PLI failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> forwardedPli;
    if (!ReceiveUdpPacket(senderSocket, &forwardedPli, 1000)) {
        std::cerr << "Expected forwarded PLI packet to publisher\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    const bool validPli = forwardedPli.size() == pliPacket.size() &&
                          forwardedPli[0] == 0x81 &&
                          forwardedPli[1] == 0xCE &&
                          forwardedPli[2] == 0x00 &&
                          forwardedPli[3] == 0x02 &&
                          forwardedPli[8] == 0x11 &&
                          forwardedPli[9] == 0x11 &&
                          forwardedPli[10] == 0x11 &&
                          forwardedPli[11] == 0x11;
    if (!validPli) {
        std::cerr << "Forwarded PLI packet mismatch\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    CloseSocket(senderSocket);
    CloseSocket(subscriberSocket);
    return true;
}

bool TestMediaIngressCoalescesFrequentPli() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-pli-coalesce");
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

    auto room = roomManager->GetRoomShared("room-pli-coalesce");
    if (!room) {
        std::cerr << "Room should exist before pli coalesce test\n";
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
    addReq.set_meeting_id("room-pli-coalesce");
    addReq.set_user_id("alice");
    addReq.set_audio_ssrc(0x11111111U);
    if (!SerializeMessage(addReq, &payload) ||
        !service.Dispatch(sfu::RpcMethod::kAddPublisher, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch add failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    sfu_rpc::AddPublisherRsp addRsp;
    if (!ParseMessage(responseBytes, &addRsp) || !addRsp.success() || service.MediaPort() == 0) {
        std::cerr << "AddPublisher should return a live UDP port\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    SocketHandle senderSocket = kInvalidSocket;
    uint16_t senderPort = 0;
    if (!CreateBoundUdpSocket(&senderSocket, &senderPort)) {
        std::cerr << "CreateBoundUdpSocket for sender failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    const std::vector<uint8_t> rtpPacket = {
        0x80, 0x60,
        0x00, 0x2A,
        0x00, 0x00, 0x00, 0x64,
        0x11, 0x11, 0x11, 0x11,
        0xAB, 0xCD, 0xEF
    };
    if (!SendUdpPacket(senderSocket, service.MediaPort(), rtpPacket.data(), rtpPacket.size())) {
        std::cerr << "SendUdpPacket RTP failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> forwardedPacket;
    if (!ReceiveUdpPacket(subscriberSocket, &forwardedPacket)) {
        std::cerr << "Expected initial forwarded RTP packet\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    const std::vector<uint8_t> pliPacket = {
        0x81, 0xCE, 0x00, 0x02,
        0x22, 0x22, 0x22, 0x22,
        0x22, 0x22, 0x22, 0x22,
    };

    auto validateForwardedPli = [&](const std::vector<uint8_t>& packet) {
        return packet.size() == pliPacket.size() &&
               packet[0] == 0x81 &&
               packet[1] == 0xCE &&
               packet[2] == 0x00 &&
               packet[3] == 0x02 &&
               packet[8] == 0x11 &&
               packet[9] == 0x11 &&
               packet[10] == 0x11 &&
               packet[11] == 0x11;
    };

    if (!SendUdpPacket(subscriberSocket, service.MediaPort(), pliPacket.data(), pliPacket.size())) {
        std::cerr << "SendUdpPacket first PLI failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> firstForwardedPli;
    if (!ReceiveUdpPacket(senderSocket, &firstForwardedPli, 1000) || !validateForwardedPli(firstForwardedPli)) {
        std::cerr << "Expected first forwarded PLI packet to publisher\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    if (!SendUdpPacket(subscriberSocket, service.MediaPort(), pliPacket.data(), pliPacket.size())) {
        std::cerr << "SendUdpPacket second PLI failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> suppressedPli;
    if (ReceiveUdpPacket(senderSocket, &suppressedPli, 150)) {
        std::cerr << "Second PLI inside cooldown should be coalesced\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    if (!SendUdpPacket(subscriberSocket, service.MediaPort(), pliPacket.data(), pliPacket.size())) {
        std::cerr << "SendUdpPacket third PLI failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> thirdForwardedPli;
    if (!ReceiveUdpPacket(senderSocket, &thirdForwardedPli, 1000) || !validateForwardedPli(thirdForwardedPli)) {
        std::cerr << "Expected third PLI after cooldown to be forwarded\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    CloseSocket(senderSocket);
    CloseSocket(subscriberSocket);
    return true;
}

bool TestMediaIngressForwardsRembToPublisher() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-remb");
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

    auto room = roomManager->GetRoomShared("room-remb");
    if (!room) {
        std::cerr << "Room should exist before remb test\n";
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
    addReq.set_meeting_id("room-remb");
    addReq.set_user_id("alice");
    addReq.set_audio_ssrc(0x11111111U);
    if (!SerializeMessage(addReq, &payload) ||
        !service.Dispatch(sfu::RpcMethod::kAddPublisher, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch add failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    sfu_rpc::AddPublisherRsp addRsp;
    if (!ParseMessage(responseBytes, &addRsp) || !addRsp.success() || service.MediaPort() == 0) {
        std::cerr << "AddPublisher should return a live UDP port\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    SocketHandle senderSocket = kInvalidSocket;
    uint16_t senderPort = 0;
    if (!CreateBoundUdpSocket(&senderSocket, &senderPort)) {
        std::cerr << "CreateBoundUdpSocket for sender failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    const std::vector<uint8_t> rtpPacket = {
        0x80, 0x60,
        0x00, 0x2A,
        0x00, 0x00, 0x00, 0x64,
        0x11, 0x11, 0x11, 0x11,
        0xAB, 0xCD, 0xEF
    };
    if (!SendUdpPacket(senderSocket, service.MediaPort(), rtpPacket.data(), rtpPacket.size())) {
        std::cerr << "SendUdpPacket RTP failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> forwardedPacket;
    if (!ReceiveUdpPacket(subscriberSocket, &forwardedPacket)) {
        std::cerr << "Expected initial forwarded RTP packet\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    const std::vector<uint8_t> rembPacket = {
        0x8F, 0xCE, 0x00, 0x05,
        0x22, 0x22, 0x22, 0x22,
        0x00, 0x00, 0x00, 0x00,
        'R', 'E', 'M', 'B',
        0x01,
        0x03, 0x0D, 0x40,
        0x22, 0x22, 0x22, 0x22,
    };
    if (!SendUdpPacket(subscriberSocket, service.MediaPort(), rembPacket.data(), rembPacket.size())) {
        std::cerr << "SendUdpPacket REMB failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> forwardedRemb;
    if (!ReceiveUdpPacket(senderSocket, &forwardedRemb, 1000)) {
        std::cerr << "Expected forwarded REMB packet to publisher\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    const bool validRemb = forwardedRemb.size() == rembPacket.size() &&
                           forwardedRemb[0] == 0x8F &&
                           forwardedRemb[1] == 0xCE &&
                           forwardedRemb[2] == 0x00 &&
                           forwardedRemb[3] == 0x05 &&
                           forwardedRemb[12] == 'R' &&
                           forwardedRemb[13] == 'E' &&
                           forwardedRemb[14] == 'M' &&
                           forwardedRemb[15] == 'B' &&
                           forwardedRemb[16] == 0x01 &&
                           forwardedRemb[17] == 0x03 &&
                           forwardedRemb[18] == 0x0D &&
                           forwardedRemb[19] == 0x40 &&
                           forwardedRemb[20] == 0x11 &&
                           forwardedRemb[21] == 0x11 &&
                           forwardedRemb[22] == 0x11 &&
                           forwardedRemb[23] == 0x11;
    if (!validRemb) {
        std::cerr << "Forwarded REMB packet mismatch\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    CloseSocket(senderSocket);
    CloseSocket(subscriberSocket);
    return true;
}

bool TestMediaIngressUsesVideoRewriteAndFeedbackMapping() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-video-feedback");
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

    auto room = roomManager->GetRoomShared("room-video-feedback");
    if (!room) {
        std::cerr << "Room should exist before video rewrite test\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    auto subscriber = std::make_shared<sfu::Subscriber>("bob", "127.0.0.1:" + std::to_string(subscriberPort), 0x22222222U, 0x33333333U);
    if (!room->AddSubscriber(subscriber)) {
        std::cerr << "AddSubscriber failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    sfu_rpc::AddPublisherReq addReq;
    addReq.set_meeting_id("room-video-feedback");
    addReq.set_user_id("alice");
    addReq.set_audio_ssrc(0x11111111U);
    addReq.set_video_ssrc(0x44444444U);
    if (!SerializeMessage(addReq, &payload) ||
        !service.Dispatch(sfu::RpcMethod::kAddPublisher, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch add failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    sfu_rpc::AddPublisherRsp addRsp;
    if (!ParseMessage(responseBytes, &addRsp) || !addRsp.success() || service.MediaPort() == 0) {
        std::cerr << "AddPublisher should return a live UDP port\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    SocketHandle senderSocket = kInvalidSocket;
    uint16_t senderPort = 0;
    if (!CreateBoundUdpSocket(&senderSocket, &senderPort)) {
        std::cerr << "CreateBoundUdpSocket for sender failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    const std::vector<uint8_t> videoRtpPacket = {
        0x80, 0x61,
        0x00, 0x2A,
        0x00, 0x00, 0x10, 0x00,
        0x44, 0x44, 0x44, 0x44,
        0xAB, 0xCD, 0xEF
    };
    if (!SendUdpPacket(senderSocket, service.MediaPort(), videoRtpPacket.data(), videoRtpPacket.size())) {
        std::cerr << "SendUdpPacket video RTP failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> forwardedVideoPacket;
    if (!ReceiveUdpPacket(subscriberSocket, &forwardedVideoPacket)) {
        std::cerr << "Expected forwarded video RTP packet\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }
    if (forwardedVideoPacket.size() != videoRtpPacket.size() ||
        forwardedVideoPacket[8] != 0x33 || forwardedVideoPacket[9] != 0x33 ||
        forwardedVideoPacket[10] != 0x33 || forwardedVideoPacket[11] != 0x33) {
        std::cerr << "Video RTP should be rewritten with subscriber video SSRC\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    const std::vector<uint8_t> pliPacket = {
        0x81, 0xCE, 0x00, 0x02,
        0x66, 0x66, 0x66, 0x66,
        0x33, 0x33, 0x33, 0x33,
    };
    if (!SendUdpPacket(subscriberSocket, service.MediaPort(), pliPacket.data(), pliPacket.size())) {
        std::cerr << "SendUdpPacket video PLI failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> forwardedPli;
    if (!ReceiveUdpPacket(senderSocket, &forwardedPli, 1000)) {
        std::cerr << "Expected forwarded video PLI packet\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }
    if (forwardedPli.size() != pliPacket.size() ||
        forwardedPli[8] != 0x44 || forwardedPli[9] != 0x44 ||
        forwardedPli[10] != 0x44 || forwardedPli[11] != 0x44) {
        std::cerr << "Video PLI should target publisher video SSRC\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    const std::vector<uint8_t> rembPacket = {
        0x8F, 0xCE, 0x00, 0x05,
        0x66, 0x66, 0x66, 0x66,
        0x00, 0x00, 0x00, 0x00,
        'R', 'E', 'M', 'B',
        0x01,
        0x03, 0x0D, 0x40,
        0x33, 0x33, 0x33, 0x33,
    };
    if (!SendUdpPacket(subscriberSocket, service.MediaPort(), rembPacket.data(), rembPacket.size())) {
        std::cerr << "SendUdpPacket video REMB failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> forwardedRemb;
    if (!ReceiveUdpPacket(senderSocket, &forwardedRemb, 1000)) {
        std::cerr << "Expected forwarded video REMB packet\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }
    if (forwardedRemb.size() != rembPacket.size() ||
        forwardedRemb[20] != 0x44 || forwardedRemb[21] != 0x44 ||
        forwardedRemb[22] != 0x44 || forwardedRemb[23] != 0x44) {
        std::cerr << "Video REMB should target publisher video SSRC\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    CloseSocket(senderSocket);
    CloseSocket(subscriberSocket);
    return true;
}

bool TestMediaIngressIsolatesFeedbackAcrossPublishers() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-feedback-isolation");
    createReq.set_max_publishers(4);
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

    auto room = roomManager->GetRoomShared("room-feedback-isolation");
    if (!room) {
        std::cerr << "Room should exist before feedback isolation test\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    auto subscriber = std::make_shared<sfu::Subscriber>("bob", "127.0.0.1:" + std::to_string(subscriberPort), 0, 0x33333333U);
    if (!room->AddSubscriber(subscriber)) {
        std::cerr << "AddSubscriber failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    auto addPublisher = [&](const std::string& userId, uint32_t videoSsrc) -> bool {
        sfu_rpc::AddPublisherReq addReq;
        addReq.set_meeting_id("room-feedback-isolation");
        addReq.set_user_id(userId);
        addReq.set_audio_ssrc(0);
        addReq.set_video_ssrc(videoSsrc);
        if (!SerializeMessage(addReq, &payload) ||
            !service.Dispatch(sfu::RpcMethod::kAddPublisher, payload.data(), payload.size(), &responseBytes)) {
            return false;
        }
        sfu_rpc::AddPublisherRsp addRsp;
        return ParseMessage(responseBytes, &addRsp) && addRsp.success() && service.MediaPort() == service.MediaPort();
    };

    constexpr uint32_t kAliceVideoSsrc = 0x44444444U;
    constexpr uint32_t kCharlieVideoSsrc = 0x55555555U;
    if (!addPublisher("alice", kAliceVideoSsrc) || !addPublisher("charlie", kCharlieVideoSsrc)) {
        std::cerr << "AddPublisher for isolation test failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    SocketHandle aliceSocket = kInvalidSocket;
    uint16_t alicePort = 0;
    if (!CreateBoundUdpSocket(&aliceSocket, &alicePort)) {
        std::cerr << "CreateBoundUdpSocket for alice failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    SocketHandle charlieSocket = kInvalidSocket;
    uint16_t charliePort = 0;
    if (!CreateBoundUdpSocket(&charlieSocket, &charliePort)) {
        std::cerr << "CreateBoundUdpSocket for charlie failed\n";
        CloseSocket(aliceSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    const std::vector<uint8_t> aliceVideoPacket = {
        0x80, 0x61,
        0x00, 0x10,
        0x00, 0x00, 0x20, 0x00,
        0x44, 0x44, 0x44, 0x44,
        0xA1, 0xA2, 0xA3
    };
    const std::vector<uint8_t> charlieVideoPacket = {
        0x80, 0x61,
        0x00, 0x20,
        0x00, 0x00, 0x30, 0x00,
        0x55, 0x55, 0x55, 0x55,
        0xB1, 0xB2, 0xB3
    };
    if (!SendUdpPacket(aliceSocket, service.MediaPort(), aliceVideoPacket.data(), aliceVideoPacket.size()) ||
        !SendUdpPacket(charlieSocket, service.MediaPort(), charlieVideoPacket.data(), charlieVideoPacket.size())) {
        std::cerr << "SendUdpPacket video RTP failed\n";
        CloseSocket(charlieSocket);
        CloseSocket(aliceSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    bool sawAlice = false;
    bool sawCharlie = false;
    uint32_t aliceRewrittenSsrc = 0U;
    uint32_t charlieRewrittenSsrc = 0U;
    auto readPacketSsrc = [](const std::vector<uint8_t>& packet) -> uint32_t {
        if (packet.size() < 12U) {
            return 0U;
        }
        return (static_cast<uint32_t>(packet[8]) << 24U) |
               (static_cast<uint32_t>(packet[9]) << 16U) |
               (static_cast<uint32_t>(packet[10]) << 8U) |
               static_cast<uint32_t>(packet[11]);
    };
    for (int i = 0; i < 4 && (!sawAlice || !sawCharlie); ++i) {
        std::vector<uint8_t> forwarded;
        if (!ReceiveUdpPacket(subscriberSocket, &forwarded, 500)) {
            continue;
        }
        const uint16_t sequence =
            forwarded.size() >= 4U
                ? static_cast<uint16_t>((static_cast<uint16_t>(forwarded[2]) << 8U) | forwarded[3])
                : 0U;
        const uint32_t rewrittenSsrc = readPacketSsrc(forwarded);
        if (sequence == 0x0010U) {
            sawAlice = true;
            aliceRewrittenSsrc = rewrittenSsrc;
        } else if (sequence == 0x0020U) {
            sawCharlie = true;
            charlieRewrittenSsrc = rewrittenSsrc;
        }
    }
    if (!sawAlice || !sawCharlie) {
        std::cerr << "Expected forwarded video packets for both publishers\n";
        CloseSocket(charlieSocket);
        CloseSocket(aliceSocket);
        CloseSocket(subscriberSocket);
        return false;
    }
    if (aliceRewrittenSsrc == 0U ||
        charlieRewrittenSsrc == 0U ||
        aliceRewrittenSsrc == charlieRewrittenSsrc ||
        aliceRewrittenSsrc == 0x33333333U ||
        charlieRewrittenSsrc == 0x33333333U ||
        aliceRewrittenSsrc == kAliceVideoSsrc ||
        charlieRewrittenSsrc == kCharlieVideoSsrc) {
        std::cerr << "Multiple publishers should get distinct per-route rewritten video SSRCs\n";
        CloseSocket(charlieSocket);
        CloseSocket(aliceSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    const std::vector<uint8_t> pliForCharlie = {
        0x81, 0xCE, 0x00, 0x02,
        0x66, 0x66, 0x66, 0x66,
        static_cast<uint8_t>((charlieRewrittenSsrc >> 24U) & 0xFFU),
        static_cast<uint8_t>((charlieRewrittenSsrc >> 16U) & 0xFFU),
        static_cast<uint8_t>((charlieRewrittenSsrc >> 8U) & 0xFFU),
        static_cast<uint8_t>(charlieRewrittenSsrc & 0xFFU),
    };
    if (!SendUdpPacket(subscriberSocket, service.MediaPort(), pliForCharlie.data(), pliForCharlie.size())) {
        std::cerr << "SendUdpPacket PLI for charlie failed\n";
        CloseSocket(charlieSocket);
        CloseSocket(aliceSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> charliePli;
    if (!ReceiveUdpPacket(charlieSocket, &charliePli, 1000)) {
        std::cerr << "Expected PLI forwarded to charlie publisher endpoint\n";
        CloseSocket(charlieSocket);
        CloseSocket(aliceSocket);
        CloseSocket(subscriberSocket);
        return false;
    }
    if (charliePli.size() != pliForCharlie.size() ||
        charliePli[8] != 0x55 || charliePli[9] != 0x55 ||
        charliePli[10] != 0x55 || charliePli[11] != 0x55) {
        std::cerr << "PLI forwarded to charlie has unexpected media SSRC\n";
        CloseSocket(charlieSocket);
        CloseSocket(aliceSocket);
        CloseSocket(subscriberSocket);
        return false;
    }
    std::vector<uint8_t> unexpectedAlicePli;
    if (ReceiveUdpPacket(aliceSocket, &unexpectedAlicePli, 150)) {
        std::cerr << "PLI for charlie should not be forwarded to alice endpoint\n";
        CloseSocket(charlieSocket);
        CloseSocket(aliceSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    const std::vector<uint8_t> rembForAlice = {
        0x8F, 0xCE, 0x00, 0x05,
        0x66, 0x66, 0x66, 0x66,
        0x00, 0x00, 0x00, 0x00,
        'R', 'E', 'M', 'B',
        0x01,
        0x03, 0x0D, 0x40,
        static_cast<uint8_t>((aliceRewrittenSsrc >> 24U) & 0xFFU),
        static_cast<uint8_t>((aliceRewrittenSsrc >> 16U) & 0xFFU),
        static_cast<uint8_t>((aliceRewrittenSsrc >> 8U) & 0xFFU),
        static_cast<uint8_t>(aliceRewrittenSsrc & 0xFFU),
    };
    if (!SendUdpPacket(subscriberSocket, service.MediaPort(), rembForAlice.data(), rembForAlice.size())) {
        std::cerr << "SendUdpPacket REMB for alice failed\n";
        CloseSocket(charlieSocket);
        CloseSocket(aliceSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> aliceRemb;
    if (!ReceiveUdpPacket(aliceSocket, &aliceRemb, 1000)) {
        std::cerr << "Expected REMB forwarded to alice publisher endpoint\n";
        CloseSocket(charlieSocket);
        CloseSocket(aliceSocket);
        CloseSocket(subscriberSocket);
        return false;
    }
    if (aliceRemb.size() != rembForAlice.size() ||
        aliceRemb[20] != 0x44 || aliceRemb[21] != 0x44 ||
        aliceRemb[22] != 0x44 || aliceRemb[23] != 0x44) {
        std::cerr << "REMB forwarded to alice has unexpected target SSRC\n";
        CloseSocket(charlieSocket);
        CloseSocket(aliceSocket);
        CloseSocket(subscriberSocket);
        return false;
    }
    std::vector<uint8_t> unexpectedCharlieRemb;
    if (ReceiveUdpPacket(charlieSocket, &unexpectedCharlieRemb, 150)) {
        std::cerr << "REMB for alice should not be forwarded to charlie endpoint\n";
        CloseSocket(charlieSocket);
        CloseSocket(aliceSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    const std::vector<uint8_t> nackForCharlie = {
        0x81, 0xCD, 0x00, 0x03,
        0x77, 0x77, 0x77, 0x77,
        static_cast<uint8_t>((charlieRewrittenSsrc >> 24U) & 0xFFU),
        static_cast<uint8_t>((charlieRewrittenSsrc >> 16U) & 0xFFU),
        static_cast<uint8_t>((charlieRewrittenSsrc >> 8U) & 0xFFU),
        static_cast<uint8_t>(charlieRewrittenSsrc & 0xFFU),
        0x00, 0x20, 0x00, 0x00,
    };
    if (!SendUdpPacket(subscriberSocket, service.MediaPort(), nackForCharlie.data(), nackForCharlie.size())) {
        std::cerr << "SendUdpPacket NACK for charlie failed\n";
        CloseSocket(charlieSocket);
        CloseSocket(aliceSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> retransmit;
    if (!ReceiveUdpPacket(subscriberSocket, &retransmit, 1000)) {
        std::cerr << "Expected NACK retransmit packet\n";
        CloseSocket(charlieSocket);
        CloseSocket(aliceSocket);
        CloseSocket(subscriberSocket);
        return false;
    }
    if (retransmit.size() != charlieVideoPacket.size() ||
        retransmit[2] != 0x00 || retransmit[3] != 0x20 ||
        readPacketSsrc(retransmit) != charlieRewrittenSsrc) {
        std::cerr << "NACK retransmit should target charlie stream only\n";
        CloseSocket(charlieSocket);
        CloseSocket(aliceSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    CloseSocket(charlieSocket);
    CloseSocket(aliceSocket);
    CloseSocket(subscriberSocket);
    return true;
}

bool TestMediaIngressIsolatesFeedbackAcrossSubscribers() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-subscriber-feedback-isolation");
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

    SocketHandle subscriberASocket = kInvalidSocket;
    uint16_t subscriberAPort = 0;
    if (!CreateBoundUdpSocket(&subscriberASocket, &subscriberAPort)) {
        std::cerr << "CreateBoundUdpSocket for subscriber A failed\n";
        return false;
    }

    SocketHandle subscriberBSocket = kInvalidSocket;
    uint16_t subscriberBPort = 0;
    if (!CreateBoundUdpSocket(&subscriberBSocket, &subscriberBPort)) {
        std::cerr << "CreateBoundUdpSocket for subscriber B failed\n";
        CloseSocket(subscriberASocket);
        return false;
    }

    auto room = roomManager->GetRoomShared("room-subscriber-feedback-isolation");
    if (!room) {
        std::cerr << "Room should exist before subscriber isolation test\n";
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }

    auto subscriberA = std::make_shared<sfu::Subscriber>("bob",
                                                         "127.0.0.1:" + std::to_string(subscriberAPort),
                                                         0,
                                                         0x55555555U);
    auto subscriberB = std::make_shared<sfu::Subscriber>("charlie",
                                                         "127.0.0.1:" + std::to_string(subscriberBPort),
                                                         0,
                                                         0x66666666U);
    if (!room->AddSubscriber(subscriberA) || !room->AddSubscriber(subscriberB)) {
        std::cerr << "AddSubscriber failed for subscriber isolation test\n";
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }

    sfu_rpc::AddPublisherReq addReq;
    addReq.set_meeting_id("room-subscriber-feedback-isolation");
    addReq.set_user_id("alice");
    addReq.set_audio_ssrc(0);
    addReq.set_video_ssrc(0x44444444U);
    if (!SerializeMessage(addReq, &payload) ||
        !service.Dispatch(sfu::RpcMethod::kAddPublisher, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch add publisher failed\n";
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }

    sfu_rpc::AddPublisherRsp addRsp;
    if (!ParseMessage(responseBytes, &addRsp) || !addRsp.success() || service.MediaPort() == 0) {
        std::cerr << "AddPublisher should return a live UDP port\n";
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }

    SocketHandle senderSocket = kInvalidSocket;
    uint16_t senderPort = 0;
    if (!CreateBoundUdpSocket(&senderSocket, &senderPort)) {
        std::cerr << "CreateBoundUdpSocket for sender failed\n";
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }

    const std::vector<uint8_t> videoPacket = {
        0x80, 0x61,
        0x00, 0x30,
        0x00, 0x00, 0x40, 0x00,
        0x44, 0x44, 0x44, 0x44,
        0xC1, 0xC2, 0xC3
    };
    if (!SendUdpPacket(senderSocket, service.MediaPort(), videoPacket.data(), videoPacket.size())) {
        std::cerr << "SendUdpPacket video RTP failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }

    std::vector<uint8_t> forwardedA;
    std::vector<uint8_t> forwardedB;
    if (!ReceiveUdpPacket(subscriberASocket, &forwardedA, 1000) ||
        !ReceiveUdpPacket(subscriberBSocket, &forwardedB, 1000)) {
        std::cerr << "Expected forwarded RTP packet on both subscriber endpoints\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }

    const bool validForwardA = forwardedA.size() == videoPacket.size() &&
                               forwardedA[8] == 0x55 && forwardedA[9] == 0x55 &&
                               forwardedA[10] == 0x55 && forwardedA[11] == 0x55;
    const bool validForwardB = forwardedB.size() == videoPacket.size() &&
                               forwardedB[8] == 0x66 && forwardedB[9] == 0x66 &&
                               forwardedB[10] == 0x66 && forwardedB[11] == 0x66;
    if (!validForwardA || !validForwardB) {
        std::cerr << "Per-subscriber RTP SSRC rewrite mismatch\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }

    const std::vector<uint8_t> pliFromSubscriberA = {
        0x81, 0xCE, 0x00, 0x02,
        0x77, 0x77, 0x77, 0x77,
        0x55, 0x55, 0x55, 0x55,
    };
    if (!SendUdpPacket(subscriberASocket, service.MediaPort(), pliFromSubscriberA.data(), pliFromSubscriberA.size())) {
        std::cerr << "SendUdpPacket PLI from subscriber A failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }

    std::vector<uint8_t> forwardedPli;
    if (!ReceiveUdpPacket(senderSocket, &forwardedPli, 1000)) {
        std::cerr << "Expected forwarded PLI packet to publisher endpoint\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }
    if (forwardedPli.size() != pliFromSubscriberA.size() ||
        forwardedPli[8] != 0x44 || forwardedPli[9] != 0x44 ||
        forwardedPli[10] != 0x44 || forwardedPli[11] != 0x44) {
        std::cerr << "PLI forwarded from subscriber A should target publisher source SSRC\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }

    const std::vector<uint8_t> rembFromSubscriberB = {
        0x8F, 0xCE, 0x00, 0x05,
        0x88, 0x88, 0x88, 0x88,
        0x00, 0x00, 0x00, 0x00,
        'R', 'E', 'M', 'B',
        0x01,
        0x03, 0x0D, 0x40,
        0x66, 0x66, 0x66, 0x66,
    };
    if (!SendUdpPacket(subscriberBSocket, service.MediaPort(), rembFromSubscriberB.data(), rembFromSubscriberB.size())) {
        std::cerr << "SendUdpPacket REMB from subscriber B failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }

    std::vector<uint8_t> forwardedRemb;
    if (!ReceiveUdpPacket(senderSocket, &forwardedRemb, 1000)) {
        std::cerr << "Expected forwarded REMB packet to publisher endpoint\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }
    if (forwardedRemb.size() != rembFromSubscriberB.size() ||
        forwardedRemb[20] != 0x44 || forwardedRemb[21] != 0x44 ||
        forwardedRemb[22] != 0x44 || forwardedRemb[23] != 0x44) {
        std::cerr << "REMB forwarded from subscriber B should target publisher source SSRC\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }

    const std::vector<uint8_t> highRembFromSubscriberA = {
        0x8F, 0xCE, 0x00, 0x05,
        0xAA, 0xAA, 0xAA, 0xAA,
        0x00, 0x00, 0x00, 0x00,
        'R', 'E', 'M', 'B',
        0x01,
        0x10, 0x61, 0xA8,
        0x55, 0x55, 0x55, 0x55,
    };
    if (!SendUdpPacket(subscriberASocket, service.MediaPort(), highRembFromSubscriberA.data(), highRembFromSubscriberA.size())) {
        std::cerr << "SendUdpPacket high REMB from subscriber A failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }

    std::vector<uint8_t> aggregatedRemb;
    if (!ReceiveUdpPacket(senderSocket, &aggregatedRemb, 1000)) {
        std::cerr << "Expected aggregated REMB packet to publisher endpoint\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }
    if (aggregatedRemb.size() != highRembFromSubscriberA.size() ||
        aggregatedRemb[17] != 0x03 || aggregatedRemb[18] != 0x0D || aggregatedRemb[19] != 0x40 ||
        aggregatedRemb[20] != 0x44 || aggregatedRemb[21] != 0x44 ||
        aggregatedRemb[22] != 0x44 || aggregatedRemb[23] != 0x44) {
        std::cerr << "Aggregated REMB should keep lower bitrate across subscribers\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }

    const std::vector<uint8_t> nackFromSubscriberA = {
        0x81, 0xCD, 0x00, 0x03,
        0x99, 0x99, 0x99, 0x99,
        0x55, 0x55, 0x55, 0x55,
        0x00, 0x30, 0x00, 0x00,
    };
    if (!SendUdpPacket(subscriberASocket, service.MediaPort(), nackFromSubscriberA.data(), nackFromSubscriberA.size())) {
        std::cerr << "SendUdpPacket NACK from subscriber A failed\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }

    std::vector<uint8_t> retransmitToA;
    if (!ReceiveUdpPacket(subscriberASocket, &retransmitToA, 1000)) {
        std::cerr << "Expected retransmit packet on subscriber A endpoint\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }
    if (retransmitToA.size() != videoPacket.size() ||
        retransmitToA[2] != 0x00 || retransmitToA[3] != 0x30 ||
        retransmitToA[8] != 0x55 || retransmitToA[9] != 0x55 ||
        retransmitToA[10] != 0x55 || retransmitToA[11] != 0x55) {
        std::cerr << "NACK retransmit should match subscriber A rewrite path\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }

    std::vector<uint8_t> unexpectedRetransmitToB;
    if (ReceiveUdpPacket(subscriberBSocket, &unexpectedRetransmitToB, 150)) {
        std::cerr << "NACK from subscriber A should not retransmit to subscriber B\n";
        CloseSocket(senderSocket);
        CloseSocket(subscriberBSocket);
        CloseSocket(subscriberASocket);
        return false;
    }

    CloseSocket(senderSocket);
    CloseSocket(subscriberBSocket);
    CloseSocket(subscriberASocket);
    return true;
}

bool TestMediaIngressRebindPublisherSsrc() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-rebind");
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

    auto room = roomManager->GetRoomShared("room-rebind");
    if (!room) {
        std::cerr << "Room should exist before rebind test\n";
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
    addReq.set_meeting_id("room-rebind");
    addReq.set_user_id("alice");
    addReq.set_audio_ssrc(0x11111111U);
    std::vector<uint8_t> addPayload;
    if (!SerializeMessage(addReq, &addPayload)) {
        std::cerr << "Serialize add request failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    if (!service.Dispatch(sfu::RpcMethod::kAddPublisher, addPayload.data(), addPayload.size(), &responseBytes)) {
        std::cerr << "Dispatch add failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    addReq.set_audio_ssrc(0x33333333U);
    if (!SerializeMessage(addReq, &addPayload)) {
        std::cerr << "Serialize rebound add request failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }
    if (!service.Dispatch(sfu::RpcMethod::kAddPublisher, addPayload.data(), addPayload.size(), &responseBytes)) {
        std::cerr << "Dispatch rebound add failed\n";
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

    std::vector<uint8_t> oldPacket = {
        0x80, 0x60,
        0x00, 0x10,
        0x00, 0x00, 0x00, 0x20,
        0x11, 0x11, 0x11, 0x11,
        0x01
    };
    if (!SendUdpPacket(sendSocket, service.MediaPort(), oldPacket.data(), oldPacket.size())) {
        std::cerr << "SendUdpPacket old SSRC failed\n";
        CloseSocket(sendSocket);
        CloseSocket(subscriberSocket);
        return false;
    }
    std::vector<uint8_t> forwardedPacket;
    if (ReceiveUdpPacket(subscriberSocket, &forwardedPacket, 250)) {
        std::cerr << "Old SSRC should not be forwarded after rebind\n";
        CloseSocket(sendSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> newPacket = {
        0x80, 0x60,
        0x00, 0x11,
        0x00, 0x00, 0x00, 0x21,
        0x33, 0x33, 0x33, 0x33,
        0x02
    };
    if (!SendUdpPacket(sendSocket, service.MediaPort(), newPacket.data(), newPacket.size())) {
        std::cerr << "SendUdpPacket new SSRC failed\n";
        CloseSocket(sendSocket);
        CloseSocket(subscriberSocket);
        return false;
    }
    if (!ReceiveUdpPacket(subscriberSocket, &forwardedPacket)) {
        std::cerr << "Expected forwarded RTP packet after rebind\n";
        CloseSocket(sendSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    CloseSocket(sendSocket);
    CloseSocket(subscriberSocket);
    return room->GetPublisher("alice") != nullptr && room->GetPublisher("alice")->AudioSsrc() == 0x33333333U;
}

bool TestMediaIngressPreservesAudioPublisherOnVideoUpdate() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-media-merge");
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

    auto room = roomManager->GetRoomShared("room-media-merge");
    if (!room) {
        std::cerr << "Room should exist before merge test\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    auto subscriber = std::make_shared<sfu::Subscriber>("bob", "127.0.0.1:" + std::to_string(subscriberPort), 0, 0);
    subscriber->SetAudioEndpoint("127.0.0.1:" + std::to_string(subscriberPort));
    if (!room->AddSubscriber(subscriber)) {
        std::cerr << "AddSubscriber failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    sfu_rpc::AddPublisherReq addReq;
    addReq.set_meeting_id("room-media-merge");
    addReq.set_user_id("alice");
    addReq.set_audio_ssrc(0x11111111U);
    addReq.set_video_ssrc(0);
    if (!SerializeMessage(addReq, &payload)) {
        std::cerr << "Serialize audio add request failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }
    if (!service.Dispatch(sfu::RpcMethod::kAddPublisher, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch audio add failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    sfu_rpc::AddPublisherRsp addRsp;
    if (!ParseMessage(responseBytes, &addRsp) || !addRsp.success() || service.MediaPort() == 0) {
        std::cerr << "Audio add publisher should return a live UDP port\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    addReq.set_audio_ssrc(0);
    addReq.set_video_ssrc(0x44444444U);
    if (!SerializeMessage(addReq, &payload)) {
        std::cerr << "Serialize video add request failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }
    if (!service.Dispatch(sfu::RpcMethod::kAddPublisher, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch video add failed\n";
        CloseSocket(subscriberSocket);
        return false;
    }

    sfu::RoomManager::PublisherLocation location;
    if (!service.mediaIngress()->ResolvePublisher(0x11111111U, &location) || location.publisher.userId != "alice") {
        std::cerr << "Audio SSRC should still resolve after video update\n";
        CloseSocket(subscriberSocket);
        return false;
    }
    if (!service.mediaIngress()->ResolvePublisher(0x44444444U, &location) || location.publisher.userId != "alice") {
        std::cerr << "Video SSRC should resolve after merge update\n";
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
        0x80, 0x6F,
        0x00, 0x2A,
        0x00, 0x00, 0x00, 0x64,
        0x11, 0x11, 0x11, 0x11,
        0xAB, 0xCD, 0xEF
    };

    if (!SendUdpPacket(sendSocket, service.MediaPort(), rtpPacket.data(), rtpPacket.size())) {
        std::cerr << "SendUdpPacket failed\n";
        CloseSocket(sendSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    std::vector<uint8_t> forwardedPacket;
    if (!ReceiveUdpPacket(subscriberSocket, &forwardedPacket)) {
        std::cerr << "Expected forwarded audio RTP packet after video update\n";
        CloseSocket(sendSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    if (forwardedPacket.size() != rtpPacket.size() ||
        forwardedPacket[8] != 0x11 || forwardedPacket[9] != 0x11 ||
        forwardedPacket[10] != 0x11 || forwardedPacket[11] != 0x11) {
        std::cerr << "Forwarded audio RTP packet SSRC mismatch after merge update\n";
        CloseSocket(sendSocket);
        CloseSocket(subscriberSocket);
        return false;
    }

    CloseSocket(sendSocket);
    CloseSocket(subscriberSocket);
    return true;
}

bool TestMediaIngressAppliesReceiverReportQuality() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-rr");
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

    sfu_rpc::AddPublisherReq addReq;
    addReq.set_meeting_id("room-rr");
    addReq.set_user_id("alice");
    addReq.set_audio_ssrc(0x11111111U);
    if (!SerializeMessage(addReq, &payload) ||
        !service.Dispatch(sfu::RpcMethod::kAddPublisher, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch add failed\n";
        return false;
    }

    sfu_rpc::AddPublisherRsp addRsp;
    if (!ParseMessage(responseBytes, &addRsp) || !addRsp.success() || service.MediaPort() == 0) {
        std::cerr << "AddPublisher should return a live UDP port\n";
        return false;
    }

    SocketHandle sendSocket = kInvalidSocket;
    uint16_t senderPort = 0;
    if (!CreateBoundUdpSocket(&sendSocket, &senderPort)) {
        std::cerr << "CreateBoundUdpSocket for sender failed\n";
        return false;
    }

    const std::vector<uint8_t> rtpPacket = {
        0x80, 0x60,
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x64,
        0x11, 0x11, 0x11, 0x11,
        0xAB, 0xCD, 0xEF
    };
    if (!SendUdpPacket(sendSocket, service.MediaPort(), rtpPacket.data(), rtpPacket.size())) {
        std::cerr << "SendUdpPacket RTP failed\n";
        CloseSocket(sendSocket);
        return false;
    }

    const std::vector<uint8_t> srPacket = {
        0x80, 0xC8, 0x00, 0x06,
        0x11, 0x11, 0x11, 0x11,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x64,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x03,
    };
    if (!SendUdpPacket(sendSocket, service.MediaPort(), srPacket.data(), srPacket.size())) {
        std::cerr << "SendUdpPacket SR failed\n";
        CloseSocket(sendSocket);
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    const std::vector<uint8_t> rrPacket = {
        0x81, 0xC9, 0x00, 0x07,
        0x22, 0x22, 0x22, 0x22,
        0x11, 0x11, 0x11, 0x11,
        0x40, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x10, 0x00,
        0x00, 0x00, 0x01, 0xE0,
        0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    if (!SendUdpPacket(sendSocket, service.MediaPort(), rrPacket.data(), rrPacket.size())) {
        std::cerr << "SendUdpPacket RR failed\n";
        CloseSocket(sendSocket);
        return false;
    }

    for (int i = 0; i < 20; ++i) {
        const auto snapshots = service.mediaIngress()->SnapshotPublisherTraffic();
        if (!snapshots.empty() &&
            snapshots.front().meetingId == "room-rr" &&
            snapshots.front().userId == "alice" &&
            snapshots.front().packetLoss > 0.24F &&
            snapshots.front().packetLoss < 0.26F &&
            snapshots.front().jitterMs == 10U &&
            snapshots.front().rttMs > 0U) {
            CloseSocket(sendSocket);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    const auto snapshots = service.mediaIngress()->SnapshotPublisherTraffic();
    if (!snapshots.empty()) {
        std::cerr << "Receiver Report quality did not reach MediaIngress snapshot"
                  << " loss=" << snapshots.front().packetLoss
                  << " jitterMs=" << snapshots.front().jitterMs
                  << " rttMs=" << snapshots.front().rttMs << "\n";
    } else {
        std::cerr << "Receiver Report quality did not reach MediaIngress snapshot\n";
    }
    CloseSocket(sendSocket);
    return false;
}

bool TestMediaIngressKeepsRttAtZeroWithoutMatchingSenderReport() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-no-sr");
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

    sfu_rpc::AddPublisherReq addReq;
    addReq.set_meeting_id("room-no-sr");
    addReq.set_user_id("alice");
    addReq.set_audio_ssrc(0x11111111U);
    if (!SerializeMessage(addReq, &payload) ||
        !service.Dispatch(sfu::RpcMethod::kAddPublisher, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch add failed\n";
        return false;
    }

    sfu_rpc::AddPublisherRsp addRsp;
    if (!ParseMessage(responseBytes, &addRsp) || !addRsp.success() || service.MediaPort() == 0) {
        std::cerr << "AddPublisher should return a live UDP port\n";
        return false;
    }

    SocketHandle sendSocket = kInvalidSocket;
    uint16_t senderPort = 0;
    if (!CreateBoundUdpSocket(&sendSocket, &senderPort)) {
        std::cerr << "CreateBoundUdpSocket for sender failed\n";
        return false;
    }

    const std::vector<uint8_t> rtpPacket = {
        0x80, 0x60,
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x64,
        0x11, 0x11, 0x11, 0x11,
        0xAB, 0xCD, 0xEF
    };
    if (!SendUdpPacket(sendSocket, service.MediaPort(), rtpPacket.data(), rtpPacket.size())) {
        std::cerr << "SendUdpPacket RTP failed\n";
        CloseSocket(sendSocket);
        return false;
    }

    const std::vector<uint8_t> rrPacket = {
        0x81, 0xC9, 0x00, 0x07,
        0x22, 0x22, 0x22, 0x22,
        0x11, 0x11, 0x11, 0x11,
        0x40, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x10, 0x00,
        0x00, 0x00, 0x01, 0xE0,
        0x00, 0x02, 0x00, 0x00,
        0x00, 0x00, 0x10, 0x00,
    };
    if (!SendUdpPacket(sendSocket, service.MediaPort(), rrPacket.data(), rrPacket.size())) {
        std::cerr << "SendUdpPacket RR failed\n";
        CloseSocket(sendSocket);
        return false;
    }

    for (int i = 0; i < 20; ++i) {
        const auto snapshots = service.mediaIngress()->SnapshotPublisherTraffic();
        if (!snapshots.empty() &&
            snapshots.front().meetingId == "room-no-sr" &&
            snapshots.front().userId == "alice" &&
            snapshots.front().packetLoss > 0.24F &&
            snapshots.front().packetLoss < 0.26F &&
            snapshots.front().jitterMs == 10U &&
            snapshots.front().rttMs == 0U) {
            CloseSocket(sendSocket);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    std::cerr << "Receiver Report without matching SR should keep RTT at zero\n";
    CloseSocket(sendSocket);
    return false;
}

bool TestMediaIngressDoesNotMisclassifyRtpAsRtcp() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-mux");
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

    sfu_rpc::AddPublisherReq addReq;
    addReq.set_meeting_id("room-mux");
    addReq.set_user_id("alice");
    addReq.set_audio_ssrc(0x11111111U);
    if (!SerializeMessage(addReq, &payload) ||
        !service.Dispatch(sfu::RpcMethod::kAddPublisher, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch add failed\n";
        return false;
    }

    sfu_rpc::AddPublisherRsp addRsp;
    if (!ParseMessage(responseBytes, &addRsp) || !addRsp.success() || service.MediaPort() == 0) {
        std::cerr << "AddPublisher should return a live UDP port\n";
        return false;
    }

    SocketHandle sendSocket = kInvalidSocket;
    uint16_t senderPort = 0;
    if (!CreateBoundUdpSocket(&sendSocket, &senderPort)) {
        std::cerr << "CreateBoundUdpSocket for sender failed\n";
        return false;
    }

    const std::vector<uint8_t> rtpLikeRtcpPacket = {
        0x80, 0xC9,
        0x00, 0x02,
        0x00, 0x00, 0x00, 0x64,
        0x11, 0x11, 0x11, 0x11,
        0xAB, 0xCD, 0xEF
    };
    if (!SendUdpPacket(sendSocket, service.MediaPort(), rtpLikeRtcpPacket.data(), rtpLikeRtcpPacket.size())) {
        std::cerr << "SendUdpPacket RTP/PT73 failed\n";
        CloseSocket(sendSocket);
        return false;
    }

    for (int i = 0; i < 20; ++i) {
        const auto snapshots = service.mediaIngress()->SnapshotPublisherTraffic();
        if (!snapshots.empty() &&
            snapshots.front().meetingId == "room-mux" &&
            snapshots.front().userId == "alice" &&
            snapshots.front().packetCount > 0) {
            CloseSocket(sendSocket);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    std::cerr << "RTP packet with RTCP-like byte[1] should still be counted as RTP\n";
    CloseSocket(sendSocket);
    return false;
}

bool CompleteDtlsHandshakeOverUdp(SocketHandle mediaSocket,
                                  uint16_t sfuPort,
                                  const std::string& serverFingerprint,
                                  sfu::DtlsContext& clientContext,
                                  sfu::DtlsTransport::SrtpKeyMaterial* keying) {
    if (keying == nullptr || !clientContext.Initialize()) {
        std::cerr << "Client DTLS context initialize failed\n";
        return false;
    }

    sfu::DtlsTransport clientTransport(clientContext, sfu::DtlsTransport::Role::Client);
    std::vector<std::vector<uint8_t>> clientOutgoing;
    if (!clientTransport.Start(serverFingerprint, &clientOutgoing)) {
        std::cerr << "Client DTLS start failed: " << clientTransport.LastError() << "\n";
        return false;
    }

    for (int iteration = 0; iteration < 64; ++iteration) {
        bool progressed = false;
        for (const auto& packet : clientOutgoing) {
            if (!SendUdpPacket(mediaSocket, sfuPort, packet.data(), packet.size())) {
                std::cerr << "SendUdpPacket DTLS failed\n";
                return false;
            }
            progressed = true;
        }
        clientOutgoing.clear();

        std::vector<uint8_t> response;
        const int timeoutMs = clientTransport.IsConnected() ? 50 : 1000;
        while (ReceiveUdpPacket(mediaSocket, &response, timeoutMs)) {
            if (!sfu::DtlsTransport::LooksLikeDtlsRecord(response.data(), response.size())) {
                continue;
            }
            if (!clientTransport.HandleIncomingDatagram(response.data(), response.size(), &clientOutgoing)) {
                std::cerr << "Client DTLS handle failed: " << clientTransport.LastError() << "\n";
                return false;
            }
            progressed = true;
            if (!clientOutgoing.empty()) {
                break;
            }
        }

        if (clientTransport.IsConnected() && clientOutgoing.empty()) {
            if (!clientTransport.ExportSrtpKeyMaterial(16U, 14U, keying)) {
                std::cerr << "Client DTLS exporter failed\n";
                return false;
            }
            return true;
        }
        if (!progressed) {
            std::cerr << "DTLS over UDP handshake stalled\n";
            return false;
        }
    }

    std::cerr << "DTLS over UDP handshake did not complete\n";
    return false;
}

bool TestIceLiteTransportRejectsRtpBeforeStunBinding() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);
    service.SetAdvertisedAddress("127.0.0.1:4567");

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-ice-lite");
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

    constexpr const char* kClientUfrag = "clientufrag";
    sfu::DtlsContext clientContext;
    if (!clientContext.Initialize()) {
        std::cerr << "Client DTLS context initialize failed\n";
        return false;
    }
    sfu_rpc::SetupTransportReq setupReq;
    setupReq.set_meeting_id("room-ice-lite");
    setupReq.set_user_id("alice");
    setupReq.set_publish_audio(true);
    setupReq.set_client_ice_ufrag(kClientUfrag);
    setupReq.set_client_ice_pwd("client-password");
    setupReq.set_client_dtls_fingerprint(clientContext.FingerprintSha256());
    setupReq.add_client_candidates("candidate:1 1 udp 2130706431 127.0.0.1 50000 typ host");
    if (!SerializeMessage(setupReq, &payload)) {
        std::cerr << "Serialize setup transport request failed\n";
        return false;
    }
    if (!service.Dispatch(sfu::RpcMethod::kSetupTransport, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch setup transport failed\n";
        return false;
    }

    sfu_rpc::SetupTransportRsp setupRsp;
    if (!ParseMessage(responseBytes, &setupRsp) ||
        !setupRsp.success() ||
        setupRsp.assigned_audio_ssrc() == 0 ||
        setupRsp.server_ice_ufrag().empty()) {
        std::cerr << "SetupTransport response mismatch\n";
        return false;
    }

    SocketHandle mediaSocket = kInvalidSocket;
    uint16_t mediaPortScratch = 0;
    if (!CreateBoundUdpSocket(&mediaSocket, &mediaPortScratch)) {
        std::cerr << "CreateBoundUdpSocket failed\n";
        return false;
    }
    (void)mediaPortScratch;

    const uint32_t assignedSsrc = setupRsp.assigned_audio_ssrc();
    std::vector<uint8_t> rtpPacket = {
        0x80, 0x6F,
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x64,
        static_cast<uint8_t>((assignedSsrc >> 24U) & 0xFFU),
        static_cast<uint8_t>((assignedSsrc >> 16U) & 0xFFU),
        static_cast<uint8_t>((assignedSsrc >> 8U) & 0xFFU),
        static_cast<uint8_t>(assignedSsrc & 0xFFU),
        0x01, 0x02, 0x03,
    };

    if (!SendUdpPacket(mediaSocket, service.MediaPort(), rtpPacket.data(), rtpPacket.size())) {
        std::cerr << "SendUdpPacket pre-STUN RTP failed\n";
        CloseSocket(mediaSocket);
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (service.mediaIngress()->PacketCount() != 0) {
        std::cerr << "RTP was accepted before ICE Binding\n";
        CloseSocket(mediaSocket);
        return false;
    }

    const std::vector<uint8_t> stunRequest =
        BuildStunBindingRequest(setupRsp.server_ice_ufrag() + ":" + kClientUfrag);
    if (!SendUdpPacket(mediaSocket, service.MediaPort(), stunRequest.data(), stunRequest.size())) {
        std::cerr << "SendUdpPacket STUN failed\n";
        CloseSocket(mediaSocket);
        return false;
    }
    std::vector<uint8_t> stunResponse;
    if (!ReceiveUdpPacket(mediaSocket, &stunResponse, 1000) ||
        stunResponse.size() < 20U ||
        stunResponse[0] != 0x01U ||
        stunResponse[1] != 0x01U) {
        std::cerr << "Expected STUN Binding success response\n";
        CloseSocket(mediaSocket);
        return false;
    }

    rtpPacket[3] = 0x02U;
    if (!SendUdpPacket(mediaSocket, service.MediaPort(), rtpPacket.data(), rtpPacket.size())) {
        std::cerr << "SendUdpPacket post-STUN RTP failed\n";
        CloseSocket(mediaSocket);
        return false;
    }
    std::vector<uint8_t> forwardedPacket;
    if (ReceiveUdpPacket(mediaSocket, &forwardedPacket, 150)) {
        std::cerr << "Raw RTP was accepted before DTLS-SRTP\n";
        CloseSocket(mediaSocket);
        return false;
    }

    sfu::DtlsTransport::SrtpKeyMaterial keying{};
    if (!CompleteDtlsHandshakeOverUdp(mediaSocket, service.MediaPort(), setupRsp.server_dtls_fingerprint(), clientContext, &keying)) {
        CloseSocket(mediaSocket);
        return false;
    }

    sfu::SrtpSession outbound;
    sfu::SrtpSession inbound;
    if (!outbound.Configure(keying.localKey, keying.localSalt, sfu::SrtpSession::Direction::Outbound) ||
        !inbound.Configure(keying.remoteKey, keying.remoteSalt, sfu::SrtpSession::Direction::Inbound)) {
        std::cerr << "Client SRTP sessions configure failed\n";
        CloseSocket(mediaSocket);
        return false;
    }
    rtpPacket[3] = 0x03U;
    std::vector<uint8_t> protectedRtp = rtpPacket;
    if (!outbound.ProtectRtp(&protectedRtp)) {
        std::cerr << "Client SRTP protect failed: " << outbound.LastError() << "\n";
        CloseSocket(mediaSocket);
        return false;
    }
    if (!SendUdpPacket(mediaSocket, service.MediaPort(), protectedRtp.data(), protectedRtp.size())) {
        std::cerr << "SendUdpPacket SRTP failed\n";
        CloseSocket(mediaSocket);
        return false;
    }
    if (!ReceiveUdpPacket(mediaSocket, &forwardedPacket, 1000) ||
        !inbound.UnprotectRtp(&forwardedPacket) ||
        forwardedPacket.size() < rtpPacket.size() ||
        forwardedPacket[3] != rtpPacket[3]) {
        std::cerr << "Expected SRTP forwarding after DTLS-SRTP\n";
        CloseSocket(mediaSocket);
        return false;
    }

    CloseSocket(mediaSocket);
    return service.mediaIngress()->PacketCount() == 1;
}

bool TestSrtpSessionRoundTrip() {
    const std::vector<uint8_t> masterKey(
        sfu::SrtpSession::MasterKeyLength(sfu::SrtpSession::Profile::Aes128CmSha1_80),
        0x11U);
    const std::vector<uint8_t> masterSalt(
        sfu::SrtpSession::MasterSaltLength(sfu::SrtpSession::Profile::Aes128CmSha1_80),
        0x22U);

    sfu::SrtpSession sender;
    sfu::SrtpSession receiver;
    if (!sender.Configure(masterKey,
                          masterSalt,
                          sfu::SrtpSession::Direction::Outbound,
                          0x11223344U)) {
        std::cerr << "SrtpSession sender configure failed: " << sender.LastError() << "\n";
        return false;
    }
    if (!receiver.Configure(masterKey,
                            masterSalt,
                            sfu::SrtpSession::Direction::Inbound,
                            0x11223344U)) {
        std::cerr << "SrtpSession receiver configure failed: " << receiver.LastError() << "\n";
        return false;
    }

    std::vector<uint8_t> rtpPacket = {
        0x80, 0x6F,
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x64,
        0x11, 0x22, 0x33, 0x44,
        0x01, 0x02, 0x03, 0x04,
    };
    const std::vector<uint8_t> plainRtp = rtpPacket;
    if (!sender.ProtectRtp(&rtpPacket) || rtpPacket.size() <= plainRtp.size()) {
        std::cerr << "SrtpSession ProtectRtp failed: " << sender.LastError() << "\n";
        return false;
    }
    if (!receiver.UnprotectRtp(&rtpPacket) || rtpPacket != plainRtp) {
        std::cerr << "SrtpSession UnprotectRtp failed: " << receiver.LastError() << "\n";
        return false;
    }

    std::vector<uint8_t> rtcpPacket = {
        0x80, 0xC8, 0x00, 0x06,
        0x11, 0x22, 0x33, 0x44,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x64,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x03,
    };
    const std::vector<uint8_t> plainRtcp = rtcpPacket;
    if (!sender.ProtectRtcp(&rtcpPacket) || rtcpPacket.size() <= plainRtcp.size()) {
        std::cerr << "SrtpSession ProtectRtcp failed: " << sender.LastError() << "\n";
        return false;
    }
    if (!receiver.UnprotectRtcp(&rtcpPacket) || rtcpPacket != plainRtcp) {
        std::cerr << "SrtpSession UnprotectRtcp failed: " << receiver.LastError() << "\n";
        return false;
    }

    return true;
}

bool TestDtlsTransportHandshakeAndExporter() {
    sfu::DtlsContext serverContext;
    sfu::DtlsContext clientContext;
    if (!serverContext.Initialize() || !clientContext.Initialize()) {
        std::cerr << "DtlsContext initialize failed\n";
        return false;
    }

    sfu::DtlsTransport serverTransport(serverContext, sfu::DtlsTransport::Role::Server);
    sfu::DtlsTransport clientTransport(clientContext, sfu::DtlsTransport::Role::Client);

    std::vector<std::vector<uint8_t>> serverOutgoing;
    std::vector<std::vector<uint8_t>> clientOutgoing;
    if (!serverTransport.Start(clientContext.FingerprintSha256(), &serverOutgoing)) {
        std::cerr << "Server DTLS start failed: " << serverTransport.LastError() << "\n";
        return false;
    }
    if (!clientTransport.Start(serverContext.FingerprintSha256(), &clientOutgoing)) {
        std::cerr << "Client DTLS start failed: " << clientTransport.LastError() << "\n";
        return false;
    }

    for (int iteration = 0; iteration < 64; ++iteration) {
        std::vector<std::vector<uint8_t>> nextClientOutgoing;
        std::vector<std::vector<uint8_t>> nextServerOutgoing;
        bool progressed = false;

        for (const auto& packet : clientOutgoing) {
            if (!serverTransport.HandleIncomingDatagram(packet.data(), packet.size(), &nextServerOutgoing)) {
                std::cerr << "Server DTLS handle failed: " << serverTransport.LastError() << "\n";
                return false;
            }
            progressed = true;
        }
        for (const auto& packet : serverOutgoing) {
            if (!clientTransport.HandleIncomingDatagram(packet.data(), packet.size(), &nextClientOutgoing)) {
                std::cerr << "Client DTLS handle failed: " << clientTransport.LastError() << "\n";
                return false;
            }
            progressed = true;
        }

        clientOutgoing = std::move(nextClientOutgoing);
        serverOutgoing = std::move(nextServerOutgoing);
        if (clientTransport.IsConnected() && serverTransport.IsConnected()) {
            break;
        }
        if (!progressed) {
            std::cerr << "DTLS handshake stalled\n";
            return false;
        }
    }

    if (!clientTransport.IsConnected() || !serverTransport.IsConnected()) {
        std::cerr << "DTLS handshake did not complete\n";
        return false;
    }
    if (clientTransport.SelectedSrtpProfile() != "SRTP_AES128_CM_SHA1_80" ||
        serverTransport.SelectedSrtpProfile() != "SRTP_AES128_CM_SHA1_80") {
        std::cerr << "DTLS SRTP profile mismatch\n";
        return false;
    }

    sfu::DtlsTransport::SrtpKeyMaterial clientKeying{};
    sfu::DtlsTransport::SrtpKeyMaterial serverKeying{};
    if (!clientTransport.ExportSrtpKeyMaterial(16U, 14U, &clientKeying) ||
        !serverTransport.ExportSrtpKeyMaterial(16U, 14U, &serverKeying)) {
        std::cerr << "DTLS exporter failed\n";
        return false;
    }

    return clientKeying.localKey == serverKeying.remoteKey &&
           clientKeying.remoteKey == serverKeying.localKey &&
           clientKeying.localSalt == serverKeying.remoteSalt &&
           clientKeying.remoteSalt == serverKeying.localSalt &&
           clientTransport.PeerFingerprintSha256() == serverContext.FingerprintSha256() &&
           serverTransport.PeerFingerprintSha256() == clientContext.FingerprintSha256();
}

bool TestIceLiteTransportRepliesToSenderReportAfterStunBinding() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);
    service.SetAdvertisedAddress("127.0.0.1:4567");

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-ice-rtcp");
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

    constexpr const char* kClientUfrag = "rtcpclient";
    sfu::DtlsContext clientContext;
    if (!clientContext.Initialize()) {
        std::cerr << "Client DTLS context initialize failed\n";
        return false;
    }
    sfu_rpc::SetupTransportReq setupReq;
    setupReq.set_meeting_id("room-ice-rtcp");
    setupReq.set_user_id("alice");
    setupReq.set_publish_audio(true);
    setupReq.set_client_ice_ufrag(kClientUfrag);
    setupReq.set_client_ice_pwd("client-password");
    setupReq.set_client_dtls_fingerprint(clientContext.FingerprintSha256());
    setupReq.add_client_candidates("candidate:1 1 udp 2130706431 127.0.0.1 50000 typ host");
    if (!SerializeMessage(setupReq, &payload)) {
        std::cerr << "Serialize setup transport request failed\n";
        return false;
    }
    if (!service.Dispatch(sfu::RpcMethod::kSetupTransport, payload.data(), payload.size(), &responseBytes)) {
        std::cerr << "Dispatch setup transport failed\n";
        return false;
    }

    sfu_rpc::SetupTransportRsp setupRsp;
    if (!ParseMessage(responseBytes, &setupRsp) ||
        !setupRsp.success() ||
        setupRsp.assigned_audio_ssrc() == 0 ||
        setupRsp.server_ice_ufrag().empty()) {
        std::cerr << "SetupTransport response mismatch\n";
        return false;
    }

    SocketHandle mediaSocket = kInvalidSocket;
    uint16_t mediaPortScratch = 0;
    if (!CreateBoundUdpSocket(&mediaSocket, &mediaPortScratch)) {
        std::cerr << "CreateBoundUdpSocket failed\n";
        return false;
    }
    (void)mediaPortScratch;

    const uint32_t assignedSsrc = setupRsp.assigned_audio_ssrc();
    const std::vector<uint8_t> senderReport = {
        0x80, 0xC8, 0x00, 0x06,
        static_cast<uint8_t>((assignedSsrc >> 24U) & 0xFFU),
        static_cast<uint8_t>((assignedSsrc >> 16U) & 0xFFU),
        static_cast<uint8_t>((assignedSsrc >> 8U) & 0xFFU),
        static_cast<uint8_t>(assignedSsrc & 0xFFU),
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x64,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x03,
    };

    if (!SendUdpPacket(mediaSocket, service.MediaPort(), senderReport.data(), senderReport.size())) {
        std::cerr << "SendUdpPacket pre-STUN SR failed\n";
        CloseSocket(mediaSocket);
        return false;
    }
    std::vector<uint8_t> unexpected;
    if (ReceiveUdpPacket(mediaSocket, &unexpected, 150)) {
        std::cerr << "RTCP SR should not receive RR before ICE Binding\n";
        CloseSocket(mediaSocket);
        return false;
    }

    const std::vector<uint8_t> stunRequest =
        BuildStunBindingRequest(setupRsp.server_ice_ufrag() + ":" + kClientUfrag);
    if (!SendUdpPacket(mediaSocket, service.MediaPort(), stunRequest.data(), stunRequest.size())) {
        std::cerr << "SendUdpPacket STUN failed\n";
        CloseSocket(mediaSocket);
        return false;
    }
    std::vector<uint8_t> stunResponse;
    if (!ReceiveUdpPacket(mediaSocket, &stunResponse, 1000) ||
        stunResponse.size() < 20U ||
        stunResponse[0] != 0x01U ||
        stunResponse[1] != 0x01U) {
        std::cerr << "Expected STUN Binding success response\n";
        CloseSocket(mediaSocket);
        return false;
    }

    if (!SendUdpPacket(mediaSocket, service.MediaPort(), senderReport.data(), senderReport.size())) {
        std::cerr << "SendUdpPacket post-STUN SR failed\n";
        CloseSocket(mediaSocket);
        return false;
    }
    std::vector<uint8_t> receiverReport;
    if (ReceiveUdpPacket(mediaSocket, &receiverReport, 150)) {
        std::cerr << "Raw RTCP SR should not receive RR before DTLS-SRTP\n";
        CloseSocket(mediaSocket);
        return false;
    }

    sfu::DtlsTransport::SrtpKeyMaterial keying{};
    if (!CompleteDtlsHandshakeOverUdp(mediaSocket, service.MediaPort(), setupRsp.server_dtls_fingerprint(), clientContext, &keying)) {
        CloseSocket(mediaSocket);
        return false;
    }

    sfu::SrtpSession outbound;
    sfu::SrtpSession inbound;
    if (!outbound.Configure(keying.localKey, keying.localSalt, sfu::SrtpSession::Direction::Outbound) ||
        !inbound.Configure(keying.remoteKey, keying.remoteSalt, sfu::SrtpSession::Direction::Inbound)) {
        std::cerr << "Client SRTCP sessions configure failed\n";
        CloseSocket(mediaSocket);
        return false;
    }

    std::vector<uint8_t> protectedSenderReport = senderReport;
    if (!outbound.ProtectRtcp(&protectedSenderReport)) {
        std::cerr << "Client SRTCP protect failed: " << outbound.LastError() << "\n";
        CloseSocket(mediaSocket);
        return false;
    }
    if (!SendUdpPacket(mediaSocket, service.MediaPort(), protectedSenderReport.data(), protectedSenderReport.size())) {
        std::cerr << "SendUdpPacket SRTCP SR failed\n";
        CloseSocket(mediaSocket);
        return false;
    }
    if (!ReceiveUdpPacket(mediaSocket, &receiverReport, 1000) ||
        !inbound.UnprotectRtcp(&receiverReport)) {
        std::cerr << "Expected protected RTCP RR after DTLS-SRTP\n";
        CloseSocket(mediaSocket);
        return false;
    }
    if (receiverReport.size() != 32U ||
        receiverReport[0] != 0x81U ||
        receiverReport[1] != 0xC9U) {
        std::cerr << "Unexpected RTCP RR header after ICE Binding\n";
        CloseSocket(mediaSocket);
        return false;
    }
    const uint32_t rrSourceSsrc =
        (static_cast<uint32_t>(receiverReport[8]) << 24U) |
        (static_cast<uint32_t>(receiverReport[9]) << 16U) |
        (static_cast<uint32_t>(receiverReport[10]) << 8U) |
        static_cast<uint32_t>(receiverReport[11]);
    const uint32_t rrLastSenderReport =
        (static_cast<uint32_t>(receiverReport[24]) << 24U) |
        (static_cast<uint32_t>(receiverReport[25]) << 16U) |
        (static_cast<uint32_t>(receiverReport[26]) << 8U) |
        static_cast<uint32_t>(receiverReport[27]);
    if (rrSourceSsrc != assignedSsrc ||
        rrLastSenderReport == 0U) {
        std::cerr << "Unexpected RTCP RR payload after ICE Binding\n";
        CloseSocket(mediaSocket);
        return false;
    }

    CloseSocket(mediaSocket);
    return true;
}

bool TestGetNodeStatusLifecycle() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    sfu::RpcService service(roomManager);
    service.SetAdvertisedAddress("127.0.0.1:4567");

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("status-room");
    createReq.set_max_publishers(4);
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

    sfu_rpc::AddPublisherReq addReq;
    addReq.set_meeting_id("status-room");
    addReq.set_user_id("alice");
    addReq.set_audio_ssrc(0x11111111U);
    std::vector<uint8_t> addPayload;
    if (!SerializeMessage(addReq, &addPayload)) {
        std::cerr << "Serialize add request failed\n";
        return false;
    }
    if (!service.Dispatch(sfu::RpcMethod::kAddPublisher, addPayload.data(), addPayload.size(), &responseBytes)) {
        std::cerr << "Dispatch add failed\n";
        return false;
    }

    if (!service.Dispatch(sfu::RpcMethod::kGetNodeStatus, nullptr, 0, &responseBytes)) {
        std::cerr << "Dispatch get node status failed\n";
        return false;
    }

    sfu_rpc::GetNodeStatusRsp rsp;
    if (!ParseMessage(responseBytes, &rsp)) {
        std::cerr << "Parse node status response failed\n";
        return false;
    }

    return rsp.success() &&
           rsp.sfu_address() == "127.0.0.1:4567" &&
           rsp.media_port() == service.MediaPort() &&
           rsp.room_count() == 1 &&
           rsp.publisher_count() == 1;
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

bool TestServerLoopbackGetNodeStatus() {
    auto roomManager = std::make_shared<sfu::RoomManager>();
    auto service = std::make_shared<sfu::RpcService>(roomManager);
    sfu::RpcServer server(0, service);
    if (!server.Start()) {
        std::cerr << "RpcServer Start failed for get node status\n";
        return false;
    }

    SocketHandle socketHandle = kInvalidSocket;
    if (!ConnectLoopback(server.Port(), &socketHandle)) {
        std::cerr << "ConnectLoopback failed for get node status\n";
        server.Stop();
        return false;
    }

    sfu::RpcFrame request;
    request.method = sfu::RpcMethod::kGetNodeStatus;
    request.kind = sfu::RpcFrameKind::kRequest;

    sfu::RpcFrame response;
    const bool ok = SendFrameAndReceiveResponse(socketHandle, request, &response);
    CloseSocket(socketHandle);
    server.Stop();
    if (!ok) {
        std::cerr << "Loopback get node status request failed\n";
        return false;
    }

    if (response.method != sfu::RpcMethod::kGetNodeStatus ||
        response.kind != sfu::RpcFrameKind::kResponse ||
        response.status != 0) {
        std::cerr << "Loopback get node status header mismatch\n";
        return false;
    }

    sfu_rpc::GetNodeStatusRsp rsp;
    if (!ParseMessage(response.payload, &rsp)) {
        std::cerr << "Loopback get node status payload parse failed\n";
        return false;
    }

    return rsp.success() &&
           rsp.sfu_address() == service->AdvertisedAddress() &&
           rsp.media_port() == service->MediaPort() &&
           rsp.room_count() == 0 &&
           rsp.publisher_count() == 0;
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
                    TestMediaIngressRetransmitsOnNack() &&
                    TestMediaIngressForwardsPliToPublisher() &&
                    TestMediaIngressCoalescesFrequentPli() &&
                    TestMediaIngressForwardsRembToPublisher() &&
                    TestMediaIngressUsesVideoRewriteAndFeedbackMapping() &&
                    TestMediaIngressIsolatesFeedbackAcrossPublishers() &&
                    TestMediaIngressIsolatesFeedbackAcrossSubscribers() &&
                    TestMediaIngressRebindPublisherSsrc() &&
                    TestMediaIngressPreservesAudioPublisherOnVideoUpdate() &&
                    TestMediaIngressAppliesReceiverReportQuality() &&
                    TestMediaIngressKeepsRttAtZeroWithoutMatchingSenderReport() &&
                    TestMediaIngressDoesNotMisclassifyRtpAsRtcp() &&
                    TestSrtpSessionRoundTrip() &&
                    TestDtlsTransportHandshakeAndExporter() &&
                    TestIceLiteTransportRejectsRtpBeforeStunBinding() &&
                    TestIceLiteTransportRepliesToSenderReportAfterStunBinding() &&
                    TestGetNodeStatusLifecycle() &&
                    TestServerLoopback() &&
                    TestServerLoopbackGetNodeStatus();
    CleanupSockets();

    if (!ok) {
        return 1;
    }

    std::cout << "sfu_rpc_server_tests passed\n";
    return 0;
}



