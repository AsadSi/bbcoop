/* bbcoop: online imports. With BB_SHADNET_SERVER set, the game's Net, NetCtl, Http, Ssl and Np*
 * imports resolve to the libraries ported from shadp2p (in libbbgpu, see
 * gpu/shim/bbcoop_network.cpp) instead of runtime_services.c's unplugged stubs. NpLookup, which
 * shadp2p leaves to its generic success stub, succeeds here too: its signed-out answer would
 * contradict a signed-in NpManager. Anything else unported (Trophy, dialogs, Voice) keeps the
 * runtime_services.c stubs. */
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <string.h>

int bbcoop_network_register(void);

static int online_symbol(const char *symbol) {
    static const char *const prefixes[]={"sceNet","sceHttp","sceSsl","sceNp"};
    for (size_t i=0;i<sizeof(prefixes)/sizeof(*prefixes);++i)
        if (!strncmp(symbol,prefixes[i],strlen(prefixes[i]))) return 1;
    return 0;
}

static ABI int32_t online_ok(void) { return 0; }

uintptr_t runtime_net_resolve(const char *name) {
    const char *symbol=runtime_symbol(name);
    if (!symbol || !online_symbol(symbol) || !bbcoop_network_register()) return 0;
    uintptr_t ported=bbgpu_resolve(name);
    if (!ported && !strncmp(symbol,"sceNpLookup",11)) return (uintptr_t)online_ok;
    return ported;
}
