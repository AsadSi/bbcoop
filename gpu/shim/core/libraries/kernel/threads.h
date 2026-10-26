// bbport: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>
#include "common/types.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {
class Thread {
public:
    Thread() = default;
    ~Thread() { Stop(); }
    void Run(std::function<void(std::stop_token)>&& func) {
        finished = std::make_shared<std::atomic<bool>>(false);
        thread = std::jthread([func = std::move(func), done = finished](std::stop_token stop) {
            runtime_thread_attach_host("bb:hle");
            func(stop);
            done->store(true);
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
        const auto done = finished;
        lock.unlock();
        // bbcoop, from Supermedo/bloodborne_pc edfe859 (GPL-2.0): guest code calls this (the game
        // stopping a movie) and cannot unwind a C++ exception. On Windows (libc++ on Win32 threads)
        // the join can fail although the thread is fine: wait for its function to return, so
        // nothing it uses is freed under it, then let it go.
        try {
            joining.join();
        } catch (const std::system_error& error) {
            std::fprintf(stderr, "HLE thread: join failed (%s); waiting for it to finish\n", error.what());
            for (int waited = 0; done && !done->load() && waited < 5000; ++waited) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (joining.joinable()) {
                joining.detach();
            }
        }
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
    std::shared_ptr<std::atomic<bool>> finished; // bbcoop: for a join that failed
};
} // namespace Libraries::Kernel
