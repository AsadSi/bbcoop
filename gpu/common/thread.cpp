// SPDX-License-Identifier: GPL-2.0-or-later
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#ifdef _WIN32
#include <algorithm>
#include <windows.h> // bbcoop: AccurateTimer
#endif

#include <pthread.h>
#ifdef __APPLE__
#include <pthread/qos.h> // bbcoop
#endif
#include <sched.h>

#include "common/logging/log.h"
#include "common/thread.h"

namespace Common {

void SetCurrentThreadName(const char* name) {
    SetThreadName(reinterpret_cast<void*>(pthread_self()), name);
#ifdef __APPLE__
    // bbcoop: the threads a frame passes through at user-interactive QoS, which macOS keeps on the
    // performance cores first; at the default class they lose cores to the game's threads in
    // heavy frames (Huax-1839/bloodborne-macos b0a3d62: missed 30-FPS slots 1.24 vs 1.99%, p99
    // 13.6-13.9 vs 17.8-19.3 ms). BB_PIPELINE_QOS=0: off.
    static const bool pipeline_qos = [] {
        const char* env = std::getenv("BB_PIPELINE_QOS");
        return !(env && env[0] == '0');
    }();
    static constexpr const char* pipeline[] = {"bb:GpuComm", "bb:DrawRec", "bb:DrawScan",
                                               "bb:DrawPrep", "bb:TexBind", "bb:GpuSignals",
                                               "bb:VkRecorder", "bb:Present"};
    for (const char* prefix : pipeline) {
        if (pipeline_qos && std::strncmp(name, prefix, std::strlen(prefix)) == 0) {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            break;
        }
    }
#endif
}

void SetThreadName(void* thread, const char* name) {
    // Linux takes at most 15 characters (and refuses longer names outright).
    char truncated[16];
    std::strncpy(truncated, name, sizeof(truncated) - 1);
    truncated[sizeof(truncated) - 1] = '\0';
    pthread_setname_np(reinterpret_cast<pthread_t>(thread), truncated);
}

std::string GetCurrentThreadName() {
    char name[64];
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0) {
        return "<unknown name>";
    }
    return name;
}

void SetCurrentThreadPriority(ThreadPriority priority) {
    // SCHED_OTHER has one static priority; the niceness of a thread is what can change, and
    // only downwards without privileges. Low priority work gets a higher nice value.
    if (priority == ThreadPriority::Low) {
        sched_param param{};
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
    }
}

void SetCurrentThreadRealtime(std::chrono::nanoseconds) {}

bool AccurateSleep(std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   bool interruptible) {
    timespec request{.tv_sec = time_t(duration.count() / 1'000'000'000),
                     .tv_nsec = long(duration.count() % 1'000'000'000)};
    timespec left{};
    int result;
    while ((result = nanosleep(&request, &left)) != 0 && errno == EINTR && !interruptible) {
        request = left;
    }
    if (remaining) {
        *remaining = result == 0 ? std::chrono::nanoseconds{0}
                                 : std::chrono::seconds{left.tv_sec} +
                                       std::chrono::nanoseconds{left.tv_nsec};
    }
    return result == 0;
}

AccurateTimer::AccurateTimer(std::chrono::nanoseconds period_) : period{period_} {}

void AccurateTimer::Start() {
    const auto now = std::chrono::steady_clock::now();
    if (deadline.time_since_epoch().count() == 0) {
        deadline = now;
    }
    if (deadline > now) {
#ifdef __APPLE__
        // No clock_nanosleep on Darwin; steady_clock is its monotonic clock.
        std::this_thread::sleep_until(deadline);
#elif defined(_WIN32)
        // bbcoop: winpthreads' clock_nanosleep takes CLOCK_REALTIME only (EINVAL otherwise), so this
        // never slept: the vblank thread ran unpaced. A high-resolution waitable timer per thread
        // (Windows 10 1803+), a plain one before; steady_clock is QueryPerformanceCounter there.
        static thread_local HANDLE timer = [] {
            HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS);
            return t ? t : CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }();
        for (auto left = deadline - std::chrono::steady_clock::now(); left.count() > 0;
             left = deadline - std::chrono::steady_clock::now()) {
            LARGE_INTEGER due;
            due.QuadPart = -std::max<LONGLONG>(
                1, std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100);
            if (!timer || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                std::this_thread::sleep_until(deadline);
                break;
            }
            WaitForSingleObject(timer, INFINITE);
        }
#else
        const auto since_epoch = deadline.time_since_epoch();
        const timespec at{
            .tv_sec = time_t(std::chrono::duration_cast<std::chrono::seconds>(since_epoch).count()),
            .tv_nsec = long((since_epoch % std::chrono::seconds{1}).count())};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &at, nullptr) == EINTR) {
        }
#endif
    }
}

void AccurateTimer::End() {
    const auto now = std::chrono::steady_clock::now();
    deadline += period;
    if (deadline + period < now) {
        deadline = now; // far behind: start over
    }
    total_wait = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
}

} // namespace Common
