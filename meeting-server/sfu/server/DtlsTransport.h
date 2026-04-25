#pragma once

#include "server/DtlsContext.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sfu {

class DtlsTransport final {
public:
    enum class Role {
        Client,
        Server,
    };

    struct SrtpKeyMaterial {
        std::vector<uint8_t> localKey;
        std::vector<uint8_t> remoteKey;
        std::vector<uint8_t> localSalt;
        std::vector<uint8_t> remoteSalt;
    };

    DtlsTransport(const DtlsContext& context, Role role);
    ~DtlsTransport();

    DtlsTransport(const DtlsTransport&) = delete;
    DtlsTransport& operator=(const DtlsTransport&) = delete;
    DtlsTransport(DtlsTransport&&) noexcept;
    DtlsTransport& operator=(DtlsTransport&&) noexcept;

    bool Start(const std::string& expectedPeerFingerprint,
               std::vector<std::vector<uint8_t>>* outgoingPackets = nullptr);
    bool HandleIncomingDatagram(const uint8_t* data,
                                std::size_t len,
                                std::vector<std::vector<uint8_t>>* outgoingPackets = nullptr);
    bool IsStarted() const noexcept;
    bool IsConnected() const noexcept;
    std::string SelectedSrtpProfile() const;
    std::string PeerFingerprintSha256() const;
    const std::string& LastError() const noexcept;

    bool ExportSrtpKeyMaterial(std::size_t keyLen,
                               std::size_t saltLen,
                               SrtpKeyMaterial* out) const;

    static bool LooksLikeDtlsRecord(const uint8_t* data, std::size_t len);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sfu
