// bbcoop: the one call shadp2p's http.cpp makes (naming summon capture files).
#pragma once
#include <unistd.h>
namespace Core::Debugger {
inline int GetCurrentPid() { return static_cast<int>(getpid()); }
} // namespace Core::Debugger
