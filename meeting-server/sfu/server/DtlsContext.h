#pragma once

#include <openssl/ssl.h>

#include <memory>
#include <string>

namespace sfu {

class DtlsContext final {
public:
    DtlsContext();
    ~DtlsContext();

    DtlsContext(const DtlsContext&) = delete;
    DtlsContext& operator=(const DtlsContext&) = delete;
    DtlsContext(DtlsContext&&) noexcept;
    DtlsContext& operator=(DtlsContext&&) noexcept;

    bool Initialize();
    bool IsReady() const noexcept;
    std::string FingerprintSha256() const;
    std::string LastError() const;
    SSL_CTX* RawContext() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sfu

