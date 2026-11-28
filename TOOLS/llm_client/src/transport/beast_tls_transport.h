// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Private PIMPL Boost.Beast + OpenSSL + Windows `ROOT` certificate store TLS
// transport implementing `IRawHttpTransport` without leaking network/OS headers.
#pragma once

#include "vn/llm/llm_client.h"

#include <memory>
#include <string_view>

namespace vn::llm::detail {

// Thread-compatible: concurrent const access is safe; post_json() requires
// external synchronization.
class BeastTlsHttpTransport final : public IRawHttpTransport {
public:
    BeastTlsHttpTransport();
    ~BeastTlsHttpTransport() override;

    BeastTlsHttpTransport(BeastTlsHttpTransport&&) noexcept;
    BeastTlsHttpTransport& operator=(BeastTlsHttpTransport&&) noexcept;

    BeastTlsHttpTransport(const BeastTlsHttpTransport&) = delete;
    BeastTlsHttpTransport& operator=(const BeastTlsHttpTransport&) = delete;

    [[nodiscard]] RawHttpResponse post_json(
        std::string_view target_path,
        std::string_view json_body,
        int connect_timeout_sec,
        int read_timeout_sec) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vn::llm::detail
