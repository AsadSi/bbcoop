// bbport: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include "common/types.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {
class Thread {
public:
    Thread() = default;
    ~Thread() { Stop(); }
    void Run(std::function<void(std::stop_token)>&& func) {
        thread = std::jthread([func = std::move(func)](std::stop_token stop) {
            runtime_thread_attach_host("bb:hle");
            func(stop);
        });
    }
    // A thread may stop its own Thread object (AvPlayer does); it detaches instead of joining.
    // AvPlayer joins one thread from two at once (Stop and the demuxer's end): the first takes the
    // thread and joins it, the other returns. A second join of a thread already joined waited
    // forever on macOS (the game froze when the intro movie was skipped).
    void Join() {
        std::unique_lock lock{mutex};
        if (!thread.joinable()) return;
        if (thread.get_id() == std::this_thread::get_id()) {
            thread.detach();
            return;
        }
        std::jthread joining = std::move(thread);
        lock.unlock();
        joining.join();
    }
    bool Joinable() const {
        std::scoped_lock lock{mutex};
        return thread.joinable();
    }
    void Stop() {
        {
            std::scoped_lock lock{mutex};
            if (!thread.joinable()) return;
            thread.request_stop();
        }
        Join();
    }

private:
    mutable std::mutex mutex;
    std::jthread thread;
};
} // namespace Libraries::Kernel
