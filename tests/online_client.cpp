// bbcoop: a stand-in for Bloodborne's online calls, for testing the ported network libraries
// without the game. Every function is resolved by NID through runtime_resolve, as the loader
// does for the game's imports, and called with the game's ABI.
//
//   BB_SHADNET_SERVER=<ip> BB_SHADNET_NPID=<id> BB_SHADNET_PASSWORD=<pw> online_client host|guest <room-file>
//
// First the bell's HTTP side, through sceHttp: fetch ss.info from the FromSoft server's address
// (redirected to the shadNet WebAPI), log in at the api_Login it names, then the guest posts a
// summon sign and the host lists signs until it sees one. Then the NP side: the host creates a
// room and writes its id to <room-file>; the guest reads it and joins. Both wait for NP signaling
// to establish, then exchange datagrams over a P2P socket (the game's transport for co-op
// traffic). Exit status 0 means a datagram arrived from the peer.
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <csignal>
#include <exception>
#include <execinfo.h>
#include <unistd.h>
#include "core/libraries/network/net.h"
#include "core/libraries/np/np_manager.h"
#include "core/libraries/np/np_types2.h"

extern "C" {
#include "runtime.h"
void runtime_restart(void) { abort(); }
void bbcoop_set_game_info(const char* serial, const char* title);
#include "bbcoop_sync.h"
}

using namespace Libraries::Np;
using namespace Libraries::Np::NpMatching2;
using Libraries::Net::OrbisNetSockaddrIn;
using Libraries::Np::NpManager::OrbisNpState;

namespace {

struct ImportName {
    const char *nid, *symbol;
};
const ImportName import_names[] = {
#include "import_names.inc"
};

template <typename F>
F Import(const char* symbol) {
    for (const auto& name : import_names) {
        if (!std::strcmp(name.symbol, symbol)) {
            if (const uintptr_t address = runtime_resolve(name.nid, 0)) return reinterpret_cast<F>(address);
        }
    }
    std::fprintf(stderr, "FAIL: %s does not resolve\n", symbol);
    std::exit(1);
}

std::atomic<int> context_event{0};
std::atomic<int> request_event{0};
std::atomic<int> request_error{0};
std::atomic<u64> created_room{0};
std::atomic<int> established_member{0};
std::atomic<u64> signaling_room{0};

PS4_SYSV_ABI void OnContext(OrbisNpMatching2ContextId, OrbisNpMatching2Event event, OrbisNpMatching2EventCause,
                            int error, void*) {
    std::printf("context event %#x error %#x\n", event, error);
    context_event = event;
}
PS4_SYSV_ABI void OnRequest(OrbisNpMatching2ContextId, OrbisNpMatching2RequestId, OrbisNpMatching2Event event,
                            int error, const void* data, void*) {
    std::printf("request event %#x error %#x\n", event, error);
    if (event == ORBIS_NP_MATCHING2_REQUEST_EVENT_CREATE_JOIN_ROOM && !error && data)
        created_room = static_cast<const OrbisNpMatching2CreateJoinRoomResponse*>(data)->roomData->roomId;
    request_error = error;
    request_event = event;
}
PS4_SYSV_ABI void OnRoom(OrbisNpMatching2ContextId, OrbisNpMatching2RoomId room, OrbisNpMatching2Event event,
                         const void*, void*) {
    std::printf("room %llu event %#x\n", static_cast<unsigned long long>(room), event);
}
PS4_SYSV_ABI void OnSignaling(OrbisNpMatching2ContextId, OrbisNpMatching2RoomId room,
                              OrbisNpMatching2RoomMemberId member, OrbisNpMatching2Event event, int error,
                              void*) {
    std::printf("signaling room %llu member %u event %#x error %#x\n", static_cast<unsigned long long>(room),
                member, event, error);
    if (event == ORBIS_NP_MATCHING2_SIGNALING_EVENT_ESTABLISHED) {
        signaling_room = room;
        established_member = member;
    }
}

void Backtrace(int signal) {
    void* frames[64];
    const int count = backtrace(frames, 64);
    std::fprintf(stderr, "CRASH (signal %d), backtrace:\n", signal);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    std::_Exit(128 + signal);
}

// Exit without static destructors: the NP libraries' worker threads are still running.
[[noreturn]] void Fail() {
    std::fflush(nullptr);
    std::_Exit(1);
}

template <typename Done>
bool WaitFor(Done done, int seconds, const char* what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr, "FAIL: timed out waiting for %s\n", what);
            std::fflush(nullptr);
            std::_Exit(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

std::string Base64Decode(const std::string& in) {
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int value = 0, bits = -8;
    for (char c : in) {
        const auto index = chars.find(c);
        if (index == std::string::npos) continue;
        value = (value << 6) | int(index);
        if ((bits += 6) >= 0) {
            out.push_back(char((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// The text between `open` and `close` after the first `open`, or "" when absent.
std::string Between(const std::string& text, const std::string& open, const std::string& close) {
    const auto start = text.find(open);
    if (start == std::string::npos) return {};
    const auto end = text.find(close, start + open.size());
    return end == std::string::npos ? std::string{} : text.substr(start + open.size(), end - start - open.size());
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 || (std::strcmp(argv[1], "host") && std::strcmp(argv[1], "guest"))) {
        std::fprintf(stderr, "usage: %s host|guest <room-file>\n", argv[0]);
        return 2;
    }
    const bool host = !std::strcmp(argv[1], "host");
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::signal(SIGSEGV, Backtrace);
    std::signal(SIGABRT, Backtrace);
    std::set_terminate([] { Backtrace(SIGABRT); });
    runtime_start(1);
    bbcoop_set_game_info("CUSA03173", "Bloodborne");

    auto net_init = Import<int(PS4_SYSV_ABI*)()>("sceNetInit");
    auto netctl_state = Import<int(PS4_SYSV_ABI*)(int*)>("sceNetCtlGetState");
    auto np_state = Import<s32(PS4_SYSV_ABI*)(s32, OrbisNpState*)>("sceNpGetState");
    auto np_check = Import<s32(PS4_SYSV_ABI*)()>("sceNpCheckCallback");
    auto np_id = Import<s32(PS4_SYSV_ABI*)(s32, OrbisNpId*)>("sceNpGetNpId");
    auto m2_init = Import<int(PS4_SYSV_ABI*)(OrbisNpMatching2InitializeParameter*)>("sceNpMatching2Initialize");
    auto m2_context_cb = Import<int(PS4_SYSV_ABI*)(OrbisNpMatching2ContextCallback, void*)>(
        "sceNpMatching2RegisterContextCallback");
    auto m2_create = Import<int(PS4_SYSV_ABI*)(const OrbisNpMatching2CreateContextParameter*,
                                               OrbisNpMatching2ContextId*)>("sceNpMatching2CreateContext");
    auto m2_room_cb = Import<int(PS4_SYSV_ABI*)(OrbisNpMatching2ContextId, OrbisNpMatching2RoomEventCallback,
                                                void*)>("sceNpMatching2RegisterRoomEventCallback");
    auto m2_signaling_cb = Import<int(PS4_SYSV_ABI*)(OrbisNpMatching2ContextId, OrbisNpMatching2SignalingCallback,
                                                     void*)>("sceNpMatching2RegisterSignalingCallback");
    auto m2_start = Import<int(PS4_SYSV_ABI*)(OrbisNpMatching2ContextId, u64)>("sceNpMatching2ContextStart");
    auto m2_create_join = Import<int(PS4_SYSV_ABI*)(OrbisNpMatching2ContextId, OrbisNpMatching2CreateJoinRoomRequest*,
                                                    OrbisNpMatching2RequestOptParam*, OrbisNpMatching2RequestId*)>(
        "sceNpMatching2CreateJoinRoom");
    auto m2_join = Import<int(PS4_SYSV_ABI*)(OrbisNpMatching2ContextId, OrbisNpMatching2JoinRoomRequest*,
                                             OrbisNpMatching2RequestOptParam*, OrbisNpMatching2RequestId*)>(
        "sceNpMatching2JoinRoom");
    auto m2_status = Import<s32(PS4_SYSV_ABI*)(OrbisNpMatching2ContextId, OrbisNpMatching2RoomId,
                                               OrbisNpMatching2RoomMemberId, s32*, void*, u16*)>(
        "sceNpMatching2SignalingGetConnectionStatus");
    auto net_socket = Import<int(PS4_SYSV_ABI*)(const char*, int, int, int)>("sceNetSocket");
    auto net_bind = Import<int(PS4_SYSV_ABI*)(int, const void*, u32)>("sceNetBind");
    auto net_sendto = Import<int(PS4_SYSV_ABI*)(int, const void*, u64, int, const void*, u32)>("sceNetSendto");
    auto net_recvfrom = Import<int(PS4_SYSV_ABI*)(int, void*, u64, int, void*, u32*)>("sceNetRecvfrom");

    if (net_init() < 0) { std::fprintf(stderr, "FAIL: sceNetInit\n"); Fail(); }
    int link = 0;
    netctl_state(&link);
    std::printf("NetCtl state %d\n", link);

    OrbisNpState state{};
    if (!WaitFor([&] { np_check(); np_state(1, &state); return state == OrbisNpState::SignedIn; }, 30,
                 "NP sign-in"))
        Fail();
    OrbisNpId self{};
    np_id(1, &self);
    std::printf("signed in as %s\n", self.handle.data);

    // The bell's HTTP side (sceHttp, as the game imports it).
    auto pool_create = Import<int(PS4_SYSV_ABI*)(const char*, int, int)>("sceNetPoolCreate");
    auto ssl_init = Import<int(PS4_SYSV_ABI*)(u64)>("sceSslInit");
    auto http_init = Import<int(PS4_SYSV_ABI*)(int, int, u64)>("sceHttpInit");
    auto http_template = Import<int(PS4_SYSV_ABI*)(int, const char*, int, int)>("sceHttpCreateTemplate");
    auto http_connection = Import<int(PS4_SYSV_ABI*)(int, const char*, bool)>("sceHttpCreateConnectionWithURL");
    auto http_request = Import<int(PS4_SYSV_ABI*)(int, s32, const char*, u64)>("sceHttpCreateRequestWithURL");
    auto http_header = Import<int(PS4_SYSV_ABI*)(int, const char*, const char*, s32)>("sceHttpAddRequestHeader");
    auto http_send = Import<int(PS4_SYSV_ABI*)(int, const void*, u64)>("sceHttpSendRequest");
    auto http_status = Import<int(PS4_SYSV_ABI*)(int, int*)>("sceHttpGetStatusCode");
    auto http_read = Import<int(PS4_SYSV_ABI*)(s32, void*, u64)>("sceHttpReadData");
    auto http_delete_request = Import<int(PS4_SYSV_ABI*)(int)>("sceHttpDeleteRequest");
    auto http_delete_connection = Import<int(PS4_SYSV_ABI*)(int)>("sceHttpDeleteConnection");
    const int http = http_init(pool_create("bbcoop-test", 0x20000, 0), ssl_init(0x40000), 0x40000);
    const int http_tmpl = http_template(http, "bbcoop-test", 2, 0);
    auto exchange = [&](bool post, const std::string& url, const std::string& body, int& status) {
        const int connection = http_connection(http_tmpl, url.c_str(), true);
        const int request = http_request(connection, post ? 1 : 0, url.c_str(), body.size());
        if (post) http_header(request, "Content-Type", "application/json", 0);
        std::string response;
        status = 0;
        if (http_send(request, body.data(), body.size()) >= 0 && http_status(request, &status) >= 0) {
            char buffer[4096];
            for (int got; (got = http_read(request, buffer, sizeof(buffer))) > 0;) response.append(buffer, got);
        }
        http_delete_request(request);
        http_delete_connection(connection);
        return response;
    };
    int status = 0;
    const std::string info = Base64Decode(exchange(false, "https://ss4.scej-network.jp:20443/eu/ss.info", "", status));
    const std::string api = Between(info, "<api_Login>", "</api_Login>");
    std::printf("ss.info: HTTP %d, api_Login at %s\n", status, api.c_str());
    if (status != 200 || api.empty()) { std::fprintf(stderr, "FAIL: ss.info\n"); Fail(); }
    const std::string login = exchange(true, api + "/basic_utils/login",
        std::string("{\"MessageId\":\"LoginRequest\",\"AuthorizationCode\":\"AUTHCODE\",\"PlatformAccountId\":") +
            (host ? "1001" : "1002") + "}", status);
    const std::string session = Between(login, "\"SessionId\":\"", "\"");
    const std::string user = Between(login, "\"UserId\":", "}");
    std::printf("login: HTTP %d session %s user %s\n", status, session.c_str(), user.c_str());
    if (status != 200 || session.empty() || user.empty()) { std::fprintf(stderr, "FAIL: login\n"); Fail(); }
    // Private servers only: anything but the shadNet server must be refused, never fetched.
    exchange(false, "http://thehuntersdream.com:18671/basic_utils/get_datetime", "", status);
    if (status != 0) { std::fprintf(stderr, "FAIL: a third-party host answered (HTTP %d)\n", status); Fail(); }
    std::printf("third-party host refused\n");
    const std::string ids = "\"SessionId\":\"" + session + "\",\"UserId\":" + user;
    const std::string area = "\"AreaId\":385875968,\"AreaRegionId\":230100,\"ChannelId\":0,"
                             "\"MatchingLevel\":46,\"SummonDataVersion\":3,\"SummonMethod\":0,"
                             "\"PosX\":143,\"PosY\":-116,\"PosZ\":-87";
    if (host) {
        const std::string search = "{\"MessageId\":\"SummonDataGetListRequest\"," + ids + "," + area +
                                   ",\"SummonTypeList\":[{\"SummonType\":0}],\"GetMaxCount\":20}";
        std::string list;
        if (!WaitFor([&] {
                list = exchange(true, api + "/summon_messenger/get", search, status);
                return list.find("\"SummonDataList\":[{") != std::string::npos;
            }, 120, "the guest's summon sign"))
            Fail();
        std::printf("summon list: %s\n", list.substr(0, 160).c_str());
    } else {
        const std::string sign = exchange(true, api + "/summon_messenger/create",
            "{\"MessageId\":\"SummonDataCreateRequest\"," + ids + "," + area +
                ",\"CharaId\":1,\"SummonData\":\"bbcoop-test-sign\",\"SummonType\":0,\"SummonWord\":null}",
            status);
        std::printf("summon sign: HTTP %d %s\n", status, sign.c_str());
        if (status != 200 || sign.find("\"ResKind\":0") == std::string::npos) {
            std::fprintf(stderr, "FAIL: summon sign\n");
            Fail();
        }
    }

    OrbisNpMatching2InitializeParameter init{};
    init.size = sizeof(init);
    if (int rc = m2_init(&init); rc < 0) { std::fprintf(stderr, "FAIL: Matching2 init %#x\n", rc); Fail(); }
    m2_context_cb(OnContext, nullptr);
    OrbisNpMatching2CreateContextParameter context_param{};
    context_param.npId = &self;
    context_param.size = sizeof(context_param);
    OrbisNpMatching2ContextId context = 0;
    if (int rc = m2_create(&context_param, &context); rc < 0) {
        std::fprintf(stderr, "FAIL: CreateContext %#x\n", rc);
        Fail();
    }
    m2_room_cb(context, OnRoom, nullptr);
    m2_signaling_cb(context, OnSignaling, nullptr);
    if (int rc = m2_start(context, 0); rc < 0) { std::fprintf(stderr, "FAIL: ContextStart %#x\n", rc); Fail(); }
    if (!WaitFor([] { return context_event == ORBIS_NP_MATCHING2_CONTEXT_EVENT_STARTED; }, 30, "context start"))
        Fail();

    OrbisNpMatching2RequestOptParam opt{};
    opt.callback = OnRequest;
    OrbisNpMatching2RequestId request = 0;
    u64 room = 0;
    if (host) {
        OrbisNpMatching2CreateJoinRoomRequest create{};
        create.maxSlot = 4;
        create.worldId = 1;
        if (int rc = m2_create_join(context, &create, &opt, &request); rc < 0) {
            std::fprintf(stderr, "FAIL: CreateJoinRoom %#x\n", rc);
            Fail();
        }
        if (!WaitFor([] { return request_event != 0; }, 30, "CreateJoinRoom reply") || request_error ||
            !created_room)
            Fail();
        room = created_room;
        std::ofstream(argv[2]) << room << '\n';
        std::printf("hosting room %llu\n", static_cast<unsigned long long>(room));
    } else {
        if (!WaitFor([&] { std::ifstream in(argv[2]); return bool(in >> room) && room; }, 60, "the host's room"))
            Fail();
        OrbisNpMatching2JoinRoomRequest join{};
        join.roomId = room;
        if (int rc = m2_join(context, &join, &opt, &request); rc < 0) {
            std::fprintf(stderr, "FAIL: JoinRoom %#x\n", rc);
            Fail();
        }
        if (!WaitFor([] { return request_event != 0; }, 30, "JoinRoom reply") || request_error) Fail();
        std::printf("joined room %llu\n", static_cast<unsigned long long>(room));
    }

    if (!WaitFor([] { return established_member != 0; }, 60, "signaling ESTABLISHED")) Fail();
    s32 connection = 0;
    u32 peer_addr = 0;
    u16 peer_port = 0;
    m2_status(context, signaling_room, OrbisNpMatching2RoomMemberId(established_member.load()), &connection,
              &peer_addr, &peer_port);
    std::printf("peer member %d status %d at %u.%u.%u.%u:%u\n", established_member.load(), connection,
                peer_addr & 0xFF, peer_addr >> 8 & 0xFF, peer_addr >> 16 & 0xFF, peer_addr >> 24, ntohs(peer_port));

    // Game traffic: a P2P datagram socket on a virtual port, addressed to the peer's endpoint.
    constexpr u16 GameVport = 3658;
    const int socket = net_socket("bbcoop-test", Libraries::Net::ORBIS_NET_AF_INET,
                                  Libraries::Net::ORBIS_NET_SOCK_DGRAM_P2P, 0);
    OrbisNetSockaddrIn local{};
    local.sin_len = sizeof(local);
    local.sin_family = Libraries::Net::ORBIS_NET_AF_INET;
    local.sin_vport = htons(GameVport);
    if (socket < 0 || net_bind(socket, &local, sizeof(local)) < 0) {
        std::fprintf(stderr, "FAIL: P2P socket/bind (%d)\n", socket);
        Fail();
    }
    OrbisNetSockaddrIn peer{};
    peer.sin_len = sizeof(peer);
    peer.sin_family = Libraries::Net::ORBIS_NET_AF_INET;
    peer.sin_addr = peer_addr;
    peer.sin_port = peer_port;
    peer.sin_vport = htons(GameVport);
    const std::string message = host ? "hello from the host" : "hello from the guest";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        net_sendto(socket, message.data(), message.size(), 0, &peer, sizeof(peer));
        char buffer[256]{};
        OrbisNetSockaddrIn from{};
        u32 from_len = sizeof(from);
        const int got = net_recvfrom(socket, buffer, sizeof(buffer) - 1, 0x80 /* DONTWAIT */, &from, &from_len);
        if (got > 0) {
            std::printf("PASS: received \"%s\"\n", buffer);
            // Keep answering briefly so the peer gets ours too.
            for (int i = 0; i < 20; ++i) {
                net_sendto(socket, message.data(), message.size(), 0, &peer, sizeof(peer));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            // The co-op sync channel (lamp travel, flags, loot): through the host's server.
            const std::string sync = host ? "coop: host lit a lamp" : "coop: guest picked up a shard";
            if (bbcoop_sync_send(sync.data(), u32(sync.size())) != 0) {
                std::fprintf(stderr, "FAIL: co-op sync send\n");
                Fail();
            }
            char from[BBCOOP_SYNC_NAME]{};
            char text[256]{};
            if (!WaitFor([&] { return bbcoop_sync_receive(from, text, sizeof(text) - 1) > 0; }, 30, "a co-op sync message"))
                Fail();
            std::printf("PASS: co-op sync from %s: \"%s\"\n", from, text);
            std::this_thread::sleep_for(std::chrono::seconds(3)); // stay in the room for the peer's
            std::fflush(nullptr);
            std::_Exit(0); // NP's worker threads are still running; skip static destructors
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::fprintf(stderr, "FAIL: no datagram from the peer\n");
    std::fflush(nullptr);
    std::_Exit(1);
}
