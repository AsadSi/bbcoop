// SPDX-FileCopyrightText: Copyright 2019-2026 rpcs3 Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/types.h"
#include "core/libraries/np/np_manager.h"
#include "core/libraries/system/userservice.h"
#include "shadnet/client.h"

namespace Libraries::Np {

class NpHandler {
public:
    static NpHandler& GetInstance();

    NpHandler(const NpHandler&) = delete;
    NpHandler& operator=(const NpHandler&) = delete;

    // Connect every currently-logged-in user that has shadNet credentials.
    void Initialize();

    // Disconnect all clients, stop worker threads, fire SignedOut for each.
    void Shutdown();

    // Connect/disconnect a single user in response to a user service login/logout event.
    void OnUserLoggedIn(s32 user_id);
    void OnUserLoggedOut(s32 user_id);

    // True if this specific user is authenticated to the shadNet server.
    bool IsPsnSignedIn(s32 user_id) const;

    /// True if any user is currently signed in
    bool IsAnySignedIn() const;

    // Set the Appear-Offline preference for all signed-in users (and future logins). While
    // enabled, shadNet handles the user as offline for everyone else. Call from the UI/config.
    void SetAppearOffline(bool enable);
    bool IsAppearOffline() const {
        return m_appear_offline.load();
    }

    /// Full NP ID for this user, built once from shadnet_npid after login.
    OrbisNpId GetNpId(s32 user_id) const;

    /// The Online ID embedded in the NP ID (npid.handle).
    OrbisNpOnlineId GetOnlineId(s32 user_id) const;

    // Avatar URL returned by the server.
    std::string GetAvatarUrl(s32 user_id) const;

    // 64-bit account ID assigned by the server.
    OrbisNpAccountId GetAccountId(s32 user_id) const;

    // WebAPI bearer token
    std::string GetBearerToken(s32 user_id) const;

    // Local IP address (network byte order) as seen at connect time.
    u32 GetLocalIpAddr(s32 user_id) const;

    // Reverse lookup: server account_id to local user_id.
    // Returns -1 if no connected user owns that account_id.
    s32 GetUserIdByAccountId(u64 account_id) const;

    // Reverse lookup: OrbisNpOnlineId to local user_id.
    // Scans m_np_ids for a matching handle.data string.
    // Returns -1 if no connected user has that Online ID.
    s32 GetUserIdByOnlineId(const OrbisNpOnlineId& online_id) const;

    // Friend list
    u32 GetNumFriends(s32 user_id) const;
    std::optional<std::string> GetFriendNpid(s32 user_id, u32 index) const;

    // ---- Friend list / requests / blocked (shadNet) ----
    struct FriendInfo {
        std::string npid;
        bool online = false;
    };
    struct FriendListSnapshot {
        std::vector<FriendInfo> friends;
        std::vector<std::string> requests_received;
        std::vector<std::string> requests_sent;
        std::vector<std::string> blocked;
    };

    // Local users that currently have an authenticated shadNet session.
    std::vector<s32> GetConnectedUsers() const;

    // user's friend state for UI display.
    FriendListSnapshot GetFriendList(s32 user_id) const;

    // Friend/block actions
    s32 SendFriendRequest(s32 user_id, const std::string& npid);
    s32 RemoveFriend(s32 user_id, const std::string& npid);
    s32 BlockUser(s32 user_id, const std::string& npid);
    s32 UnblockUser(s32 user_id, const std::string& npid);

    // State callbacks
    using StateCallback = std::function<void(Libraries::UserService::OrbisUserServiceUserId user_id,
                                             NpManager::OrbisNpState state)>;

    s32 RegisterStateCallback(StateCallback cb, void* userdata);
    void UnregisterStateCallback(s32 handle);

private:
    NpHandler() = default;
    ~NpHandler() = default;

    /// Connect one user.  Blocks until connected+authenticated or failed.
    bool ConnectUser(s32 user_id, const std::string& host, u16 port, const std::string& npid,
                     const std::string& password, const std::string& token);

    // Connect a single logged-in user by id (looks up credentials, parses server).
    bool ConnectUserById(s32 user_id);

    // Parse the configured shadNet server "host:port" (default port 31313).
    std::pair<std::string, u16> ParseServerAddress() const;

    // Start the health-monitor worker thread if not already running (idempotent).
    void StartWorker();

    // Disconnect and remove one user's client.
    void DisconnectUser(s32 user_id);

    void WorkerThread();
    // Transparent reconnect after a network drop
    void MarkForReconnect(s32 user_id);
    void TryReconnect();
    void FireStateCallback(s32 user_id, NpManager::OrbisNpState state);

    // Notification forwarders wired into each client
    void OnFriendQuery(s32 user_id, const ShadNet::NotifyFriendQuery& n);
    void OnFriendNew(s32 user_id, const ShadNet::NotifyFriendNew& n);
    void OnFriendLost(s32 user_id, const ShadNet::NotifyFriendLost& n);
    void OnFriendStatus(s32 user_id, const ShadNet::NotifyFriendStatus& n);
    void OnLoginResult(s32 user_id, const ShadNet::LoginResult& res);

    // General async reply dispatch. Routes a reply to the owning subsystem by
    // command. Called from the per-user ShadNetClient on the reader thread.
    void OnAsyncReply(s32 user_id, ShadNet::CommandType cmd, u64 pkt_id, ShadNet::ErrorType error,
                      const std::vector<u8>& body);

    // Appear-Offline preference, applied to every client at login and on change.
    std::atomic<bool> m_appear_offline{false};

    // Per-user client map
    mutable std::mutex m_mutex_clients;
    std::map<s32, std::shared_ptr<ShadNet::ShadNetClient>> m_clients;
    // Per-user NP ID built once from shadnet_npid after login.
    std::map<s32, OrbisNpId> m_np_ids;

    // Worker thread
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_worker_running{false};
    std::thread m_worker_thread;

    // Users dropped by a network error and awaiting transparent reconnect
    struct ReconnectState {
        std::chrono::steady_clock::time_point next_attempt{};
        std::chrono::milliseconds backoff{0};
    };
    std::unordered_map<s32, ReconnectState> m_reconnect;

    // State callbacks
    struct CbEntry {
        s32 handle;
        StateCallback cb;
        void* userdata;
    };
    mutable std::mutex m_mutex_cbs;
    std::vector<CbEntry> m_state_cbs;
    s32 m_next_handle{1};

    // Friend state per user, seeded from LoginReply and updated by notifications/actions.
    mutable std::mutex m_mutex_friend_state;
    std::map<s32, FriendListSnapshot> m_friend_state;
};

} // namespace Libraries::Np