// bbcoop: shadNet toasts become log lines (no overlay UI for friends/presence).
#pragma once
#include <string>
#include "common/logging/log.h"
namespace ImGui::ShadNetNotify {
enum class Kind { FriendRequest, FriendNew, FriendLost, Online, Info };
inline void Push(Kind, const std::string& text) { LOG_INFO(NpHandler, "shadNet: {}", text); }
} // namespace ImGui::ShadNetNotify
