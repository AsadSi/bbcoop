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
        std::scoped_lock lock{mutex};
        stop_source = thread.get_stop_source(); // bbcoop: see Stop()
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
            // bbcoop: the stop is asked for even when another thread is joining this one (it took
            // the jthread): the movie's demuxer joins the video decoder at the end of the file while
            // the decoder waits for a buffer the game no longer returns once it stops the movie;
            // without the stop neither returned (Windows, 2026-10-26: black screen after the intro).
            stop_source.request_stop();
            if (!thread.joinable()) return;
        }
        Join();
    }

private:
    mutable std::mutex mutex;
    std::jthread thread;
    std::stop_source stop_source{std::nostopstate}; // bbcoop: the running thread's, kept by Run
    std::shared_ptr<std::atomic<bool>> finished; // bbcoop: for a join that failed
};
} // namespace Libraries::Kernel
