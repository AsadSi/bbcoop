// bbcoop: the co-op sync channel (bbcoop_sync.h) over the shadNet client of the local user.
#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include "bbcoop_sync.h"
#include "core/libraries/np/np_handler.h"
#include "shadnet.pb.h"

namespace {
struct Message {
    std::string from, data;
};
std::mutex mutex;
std::deque<Message> inbox;
constexpr size_t InboxLimit = 1024; // a stalled reader must not grow this without bound
} // namespace

extern "C" int bbcoop_sync_send(const void* data, uint32_t size) {
    if (size > BBCOOP_SYNC_MAX) return -1;
    const auto client = Libraries::Np::NpHandler::GetInstance().GetClient(1);
    if (!client || !client->IsAuthenticated()) return -1;
    shadnet::CoopRelayRequest request;
    request.set_data(data, size);
    const std::string bytes = request.SerializeAsString();
    std::vector<u8> payload(4 + bytes.size());
    const u32 length = static_cast<u32>(bytes.size());
    std::memcpy(payload.data(), &length, 4); // u32 LE length, then the protobuf
    std::memcpy(payload.data() + 4, bytes.data(), bytes.size());
    client->SubmitRequest(ShadNet::CommandType::CoopRelay, payload);
    return 0;
}

extern "C" int bbcoop_sync_receive(char from[BBCOOP_SYNC_NAME], void* data, uint32_t capacity) {
    std::scoped_lock lock{mutex};
    if (inbox.empty()) return -1;
    const Message message = std::move(inbox.front());
    inbox.pop_front();
    const size_t name = std::min<size_t>(message.from.size(), BBCOOP_SYNC_NAME - 1);
    std::memcpy(from, message.from.data(), name);
    from[name] = '\0';
    std::memcpy(data, message.data.data(), std::min<size_t>(message.data.size(), capacity));
    return static_cast<int>(message.data.size());
}

extern "C" void bbcoop_sync_deliver(const char* from, const void* data, uint32_t size) {
    std::scoped_lock lock{mutex};
    if (inbox.size() >= InboxLimit) inbox.pop_front();
    inbox.push_back({from, std::string(static_cast<const char*>(data), size)});
}
