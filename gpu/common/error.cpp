// SPDX-License-Identifier: GPL-2.0-or-later
#include <cerrno>
#include <cstring>
#include <string>

#include "common/error.h"

namespace Common {

std::string NativeErrorToString(int e) {
    char buffer[256];
#ifdef __APPLE__
    // XSI strerror_r: the message is in `buffer`.
    return strerror_r(e, buffer, sizeof(buffer)) ? std::string{"error "} + std::to_string(e)
                                                 : std::string{buffer};
#else
    // GNU strerror_r: returns the message, which may or may not be in `buffer`.
    return std::string{strerror_r(e, buffer, sizeof(buffer))};
#endif
}

std::string GetLastErrorMsg() {
    return NativeErrorToString(errno);
}

} // namespace Common
