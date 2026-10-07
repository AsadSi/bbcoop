/* bbcoop: online imports. With BB_SHADNET_SERVER set, the game's Net, NetCtl, Http, Ssl and Np*
 * imports resolve to the libraries ported from shadp2p (in libbbgpu, see
 * gpu/shim/bbcoop_network.cpp) instead of runtime_services.c's unplugged stubs. Imports those
 * libraries do not implement still fall through to the stubs. */
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

uintptr_t runtime_net_resolve(const char *name) {
    const char *symbol=runtime_symbol(name);
    if (!symbol || !online_symbol(symbol) || !bbcoop_network_register()) return 0;
    return bbgpu_resolve(name);
}
