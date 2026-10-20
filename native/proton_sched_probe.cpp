// SPDX-License-Identifier: GPL-3.0-or-later
// What the console's scheduler does with a title's thread priorities, measured.
//
// Wine maps Windows thread priorities onto the host's; before this title maps
// anything, this probe establishes on the console itself: the priority range
// and direction, the policies and what a title may set, the priority a thread
// actually has after a change, whether threads of equal priority share a CPU,
// how late a periodic (audio-like) thread wakes while every core is busy at
// each priority, and whether a low-priority thread starves. It runs in place
// of a game when wine-run.txt carries env=PROTON_SCHED_PROBE=1, prints
// "sched:" lines to klog and returns to the launcher.
#include "proton_sched_probe.h"
#include "../ps5/src/platform.h"
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

extern "C" {
int scePthreadGetprio(pthread_t thread, int *priority);
int scePthreadSetprio(pthread_t thread, int priority);
}

namespace {

uint64_t now_us()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return uint64_t(now.tv_sec) * 1000000 + uint64_t(now.tv_nsec) / 1000;
}

struct Busy {
    std::atomic<bool> *stop;
    std::atomic<uint64_t> count{0};
    int policy = SCHED_FIFO, priority = 700;
    pthread_t thread{};
};

void set_self(int policy, int priority)
{
    sched_param param{};
    param.sched_priority = priority;
    pthread_setschedparam(pthread_self(), policy, &param);
}

void *spin(void *opaque)
{
    auto *busy = static_cast<Busy *>(opaque);
    set_self(busy->policy, busy->priority);
    uint64_t local = 0;
    while (!busy->stop->load(std::memory_order_relaxed)) {
        if ((++local & 0xffff) == 0) busy->count.store(local, std::memory_order_relaxed);
    }
    busy->count.store(local, std::memory_order_relaxed);
    return nullptr;
}

struct Periodic {
    std::atomic<bool> *stop;
    int policy = SCHED_FIFO, priority = 700;
    std::vector<uint32_t> lateness;   // us past each 2 ms deadline
};

void *periodic(void *opaque)
{
    auto *p = static_cast<Periodic *>(opaque);
    set_self(p->policy, p->priority);
    p->lateness.reserve(1000);
    uint64_t next = now_us() + 2000;
    while (!p->stop->load(std::memory_order_relaxed) && p->lateness.size() < 1000) {
        const uint64_t now = now_us();
        if (next > now) usleep(uint32_t(next - now));
        const uint64_t woke = now_us();
        p->lateness.push_back(uint32_t(woke > next ? woke - next : 0));
        next += 2000;
    }
    return nullptr;
}

uint32_t percentile(std::vector<uint32_t> values, unsigned percent)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const size_t index = std::min(values.size() - 1, (values.size() * percent + 99) / 100 - 1);
    return values[index];
}

// For one second: N busy threads (policy, priority), a 2 ms periodic thread
// and optionally one more busy thread at low_priority. The probe's own
// thread sits at the top priority meanwhile, so the phase always ends. It
// reports how many periods ran (500 expected) and how late they woke, how
// evenly the busy threads shared the CPUs and whether the low one ran.
void contention(const char *name, int busy_threads, int policy, int busy_priority, int periodic_priority,
                int low_priority)
{
    std::atomic<bool> stop{false};
    std::vector<Busy> busy(busy_threads);
    Busy low;
    Periodic tick;
    tick.stop = &stop;
    tick.policy = policy;
    tick.priority = periodic_priority;
    low.stop = &stop;
    low.policy = policy;
    low.priority = low_priority;
    set_self(SCHED_FIFO, 256);
    for (auto &b : busy) {
        b.stop = &stop;
        b.policy = policy;
        b.priority = busy_priority;
        pthread_create(&b.thread, nullptr, spin, &b);
    }
    if (low_priority) pthread_create(&low.thread, nullptr, spin, &low);
    pthread_t ticker;
    pthread_create(&ticker, nullptr, periodic, &tick);
    usleep(1000000);
    stop = true;
    pthread_join(ticker, nullptr);
    for (auto &b : busy) pthread_join(b.thread, nullptr);
    if (low_priority) pthread_join(low.thread, nullptr);
    set_self(SCHED_FIFO, 700);
    uint64_t least = UINT64_MAX, most = 0;
    for (auto &b : busy) {
        const uint64_t c = b.count.load();
        least = std::min(least, c);
        most = std::max(most, c);
    }
    say("sched: %s policy=%d busy=%d@%d periodic@%d periods=%zu lateness_us p50=%u p99=%u max=%u "
        "busy_min/max=%.3f starved=%d low@%d=%llu",
        name, policy, busy_threads, busy_priority, periodic_priority, tick.lateness.size(),
        percentile(tick.lateness, 50), percentile(tick.lateness, 99), percentile(tick.lateness, 100),
        most ? double(least) / double(most) : 0.0,
        int(std::count_if(busy.begin(), busy.end(), [](const Busy &b) { return b.count.load() == 0; })),
        low_priority, (unsigned long long)low.count.load());
}

void *idle_thread(void *) { usleep(200000); return nullptr; }

} // namespace

void proton_sched_probe()
{
    pthread_t self = pthread_self();
    int policy = -1, sce = -1;
    sched_param param{};
    pthread_getschedparam(self, &policy, &param);
    scePthreadGetprio(self, &sce);
    say("sched: main thread policy=%d sched_priority=%d sce_prio=%d", policy, param.sched_priority, sce);
    for (int p : {SCHED_FIFO, SCHED_OTHER, SCHED_RR})
        say("sched: policy %d range [%d, %d]", p, sched_get_priority_min(p), sched_get_priority_max(p));

    // A new thread's defaults, and what setting a priority does to it.
    pthread_t helper;
    pthread_create(&helper, nullptr, idle_thread, nullptr);
    int helper_policy = -1, helper_sce = -1;
    sched_param helper_param{};
    pthread_getschedparam(helper, &helper_policy, &helper_param);
    scePthreadGetprio(helper, &helper_sce);
    say("sched: new thread policy=%d sched_priority=%d sce_prio=%d", helper_policy, helper_param.sched_priority,
        helper_sce);
    for (int wanted : {256, 400, 500, 600, 650, 700, 750, 767, 255, 768, 1000}) {
        const int status = scePthreadSetprio(helper, wanted);
        int got = -1;
        scePthreadGetprio(helper, &got);
        say("sched: scePthreadSetprio(%d) -> %#x, now %d", wanted, status, got);
    }
    for (int p : {SCHED_FIFO, SCHED_RR, SCHED_OTHER}) {
        sched_param set{};
        set.sched_priority = (sched_get_priority_min(p) + sched_get_priority_max(p)) / 2;
        const int status = pthread_setschedparam(helper, p, &set);
        int got_policy = -1, got_sce = -1;
        sched_param got{};
        pthread_getschedparam(helper, &got_policy, &got);
        scePthreadGetprio(helper, &got_sce);
        say("sched: pthread_setschedparam(policy %d, %d) -> %d, now policy=%d sched_priority=%d sce_prio=%d",
            p, set.sched_priority, status, got_policy, got.sched_priority, got_sce);
    }
    pthread_join(helper, nullptr);

    // Contention: twice as many busy threads as CPUs, a 2 ms periodic thread.
    const int cores = int(sysconf(_SC_NPROCESSORS_ONLN));
    say("sched: %d CPUs online", cores);
    const int base = helper_sce > 0 ? helper_sce : 700;
    contention("idle-system", 0, SCHED_FIFO, base, base, 0);
    for (int policy : {SCHED_FIFO, SCHED_RR}) {
        contention("equal", cores * 2, policy, base, base, 0);
        contention("periodic-higher-1", cores * 2, policy, base, base - 1, 0);
        contention("periodic-higher-8", cores * 2, policy, base, base - 8, 0);
        contention("periodic-higher-32", cores * 2, policy, base, base - 32, 0);
        contention("low-plus-1", cores * 2, policy, base, base - 32, base + 1);
        contention("low-plus-16", cores * 2, policy, base, base - 32, base + 16);
    }
    // One busy thread per CPU at most: does an equal-priority periodic thread still run?
    contention("cores-equal", cores, SCHED_RR, base, base, 0);
    contention("cores-equal", cores, SCHED_FIFO, base, base, 0);
    say("sched: probe done");
}
