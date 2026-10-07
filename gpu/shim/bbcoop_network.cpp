// bbcoop: registers the ported online libraries (shadp2p: Net, NetCtl, Http, Ssl, NpCommon,
// NpManager, NpAuth, NpMatching2, NpSignaling, NpScore, NpWebApi) with bbgpu's NID table. Only
// when online (BB_SHADNET_SERVER set): offline, runtime_services.c keeps answering with its
// unplugged stubs. NpManager's registration connects to the shadNet server (NpHandler::Initialize).
#include <ctime>
#include <mutex>
#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/libraries/system/userservice.h"

namespace Libraries::Net { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::NetCtl { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Http { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Ssl { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Np::NpCommon { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Np::NpManager { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Np::NpAuth { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Np::NpMatching2 { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Np::NpSignaling { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Np::NpScore { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Np::NpWebApi { void RegisterLib(Core::Loader::SymbolsResolver* sym); }

// The two host services the ported libraries call directly rather than through an import.
namespace Libraries::UserService {
s32 PS4_SYSV_ABI sceUserServiceGetInitialUser(int* user_id) {
    *user_id = 1; // bbport's one local user (runtime_services.c)
    return 0;
}
} // namespace Libraries::UserService
namespace Libraries::Kernel {
s32 PS4_SYSV_ABI sceKernelClockGettime(u32 clock_id, OrbisKernelTimespec* tp) {
    timespec now;
    if (clock_gettime(clock_id == ORBIS_CLOCK_REALTIME ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now)) return -1;
    tp->tv_sec = now.tv_sec;
    tp->tv_nsec = now.tv_nsec;
    return 0;
}
} // namespace Libraries::Kernel

extern "C" int bbcoop_network_register(void) {
    static std::once_flag once;
    static bool online = false;
    std::call_once(once, [] {
        online = EmulatorSettings.IsShadNetEnabled();
        if (!online) return;
        Core::Loader::SymbolsResolver resolver;
        Libraries::Net::RegisterLib(&resolver);
        Libraries::NetCtl::RegisterLib(&resolver);
        Libraries::Http::RegisterLib(&resolver);
        Libraries::Ssl::RegisterLib(&resolver);
        Libraries::Np::NpCommon::RegisterLib(&resolver);
        Libraries::Np::NpAuth::RegisterLib(&resolver);
        Libraries::Np::NpMatching2::RegisterLib(&resolver);
        Libraries::Np::NpSignaling::RegisterLib(&resolver);
        Libraries::Np::NpScore::RegisterLib(&resolver);
        Libraries::Np::NpWebApi::RegisterLib(&resolver);
        Libraries::Np::NpManager::RegisterLib(&resolver); // last: connects to the server
    });
    return online;
}
