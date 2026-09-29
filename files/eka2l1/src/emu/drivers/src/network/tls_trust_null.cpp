// Copyright (c) 2026 EKA2L1 Team.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tls_trust.h"

namespace eka2l1::drivers {
    bool verify_system_certificate(const tls_certificate_chain &chain, const std::string &hostname) {
        return true;
    }
}
