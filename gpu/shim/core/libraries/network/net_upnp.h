// bbcoop: no UPnP. Friends connect over Tailscale, where ports need no router mapping.
#pragma once
#include <string>
#include "common/types.h"
namespace Libraries::Net {
class UPnPClient {
public:
    static UPnPClient& Instance() { static UPnPClient client; return client; }
    void Start() {}
    void SetP2PFeaturesEnabled(bool) {}
    bool WaitReady(int) { return false; }
    bool IsAvailable() const { return false; }
    u32 GetExternalIp() const { return 0; }
    std::string GetExternalIpString() const { return {}; }
    u16 GetExternalPort() const { return 0; }
    void AddMapping(u16) {}
    void RemoveMapping(u16) {}
};
} // namespace Libraries::Net
