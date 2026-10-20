/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../wine/ps5/pw_wine_threads.h"
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

static atomic_int ready, finish;
static void *worker(void *unused)
{
    (void)unused;
    assert(!pw_wine_thread_register(42, pthread_self()));
    atomic_store(&ready, 1);
    while (!atomic_load(&finish)) sched_yield();
    pw_wine_thread_unregister(42);
    return NULL;
}

static int remote_called;
static int remote_signal(pid_t pid, long tid, int sig)
{
    assert(pid == getpid() + 1 && tid == 77 && sig == SIGUSR1);
    ++remote_called;
    return 0;
}
static int remote_close(pid_t pid)
{
    assert(pid == getpid() + 1);
    ++remote_called;
    return 0;
}
/* The sampler: a thread that spins in user code is reported with its own instruction pointer and a CPU clock that advances,
 * and a thread that blocks the sampling signal is reported stale. */
static atomic_int spinning_ready, spinning_stop;
static volatile unsigned long spin_counter;
__attribute__((noinline)) static void spin_here(void)
{
    while (!atomic_load(&spinning_stop)) spin_counter++;
}
static void *spinner(void *unused)
{
    (void)unused;
    assert(!pw_wine_thread_register(1001, pthread_self()));
    atomic_store(&spinning_ready, 1);
    spin_here();
    pw_wine_thread_unregister(1001);
    return NULL;
}
static atomic_int masked_ready, masked_stop;
static void *masked(void *unused)
{
    (void)unused;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPROF);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    assert(!pw_wine_thread_register(1002, pthread_self()));
    atomic_store(&masked_ready, 1);
    while (!atomic_load(&masked_stop)) usleep(1000);
    pw_wine_thread_unregister(1002);
    return NULL;
}
static void sampler_case(void)
{
    pthread_t a, b;
    PwWineThreadSample out[8];
    unsigned live = 0, stale = 0, advanced = 0, found_spin = 0;
    uint64_t first_cpu = 0, last_cpu = 0;
    assert(!pthread_create(&a, NULL, spinner, NULL));
    assert(!pthread_create(&b, NULL, masked, NULL));
    while (!atomic_load(&spinning_ready) || !atomic_load(&masked_ready)) sched_yield();
    for (int i = 0; i < 20; i++) {
        const unsigned n = pw_wine_threads_sample(out, 8, 5000);
        assert(n == 2);
        for (unsigned j = 0; j < n; j++) {
            if (out[j].tid == 1001) {
                assert(!out[j].stale && out[j].rip && out[j].rsp);
                /* the instruction pointer lies in this program's own text: the spin loop's function */
                const uintptr_t spin = (uintptr_t)spin_here;
                if (out[j].rip >= spin && out[j].rip < spin + 128) found_spin++;
                if (!first_cpu) first_cpu = out[j].cpu_ns;
                last_cpu = out[j].cpu_ns;
                live++;
            } else {
                assert(out[j].tid == 1002 && out[j].stale && !out[j].rip);
                stale++;
            }
        }
        usleep(2000);
    }
    advanced = last_cpu > first_cpu;
    assert(live == 20 && stale == 20 && advanced && found_spin >= 15);
    atomic_store(&spinning_stop, 1);
    atomic_store(&masked_stop, 1);
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    assert(pw_wine_threads_sample(out, 8, 1000) == 0);
}

/* The thread start hook: run once on each registering thread, outside the registry's lock, so a hook may use the
 * registry itself (pw_wine_thread_set_priority takes its lock). */
static atomic_int hook_calls, hook_wrong_thread;
static _Thread_local long hooked_tid;
static _Thread_local int hook_ran_here;
static void start_hook(void)
{
    atomic_fetch_add(&hook_calls, 1);
    hook_ran_here++;
    if (pw_wine_thread_set_priority(hooked_tid, SCHED_OTHER, 0)) atomic_fetch_add(&hook_wrong_thread, 1);
}
static void *hooked(void *tid)
{
    hooked_tid = (long)(intptr_t)tid;
    assert(!pw_wine_thread_register(hooked_tid, pthread_self()));
    assert(hook_ran_here == 1);
    pw_wine_thread_unregister(hooked_tid);
    return NULL;
}
static void start_hook_case(void)
{
    pthread_t threads[4];
    assert(__wine_ps5_set_payload_thread_start(NULL) == -1);
    assert(atomic_load(&hook_calls) == 0);
    assert(!__wine_ps5_set_payload_thread_start(start_hook));
    alarm(30);  /* a hook run under the lock deadlocks: end in a signal, not a hang */
    for (int i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, hooked, (void *)(intptr_t)(2000 + i)));
    for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    alarm(0);
    assert(atomic_load(&hook_calls) == 4 && atomic_load(&hook_wrong_thread) == 0);
}

int main(void)
{
    assert(pw_wine_thread_set_priority(42, SCHED_OTHER, 0) == ESRCH);
    /* More thread lifetimes than registry slots; concurrently change priority
     * as each thread unregisters and exits. A stale pthread must never be used. */
    for (int i = 0; i < PW_WINE_THREADS_MAX + 10; ++i) {
        pthread_t thread;
        atomic_store(&ready, 0);
        atomic_store(&finish, 0);
        assert(!pthread_create(&thread, NULL, worker, NULL));
        while (!atomic_load(&ready)) sched_yield();
        assert(!pw_wine_thread_set_priority(42, SCHED_OTHER, 0));
        assert(pw_wine_thread_set_priority(42, -1, 0) != 0);
        atomic_store(&finish, 1);
        int status;
        do status = pw_wine_thread_set_priority(42, SCHED_OTHER, 0); while (!status);
        assert(status == ESRCH);
        assert(!pthread_join(thread, NULL));
    }
    assert(pw_wine_thread_kill(getpid()+1,77,SIGUSR1) == -1 && errno == ESRCH);
    assert(pw_wine_process_close(getpid()+1) == -1 && errno == ESRCH);
    pw_wine_threads_set_remote_signal(remote_signal,remote_close);
    assert(!pw_wine_thread_kill(getpid()+1,77,SIGUSR1) && remote_called == 1);
    assert(!pw_wine_process_close(getpid()+1) && remote_called == 2);
    pw_wine_threads_set_remote_signal(NULL,NULL);
    assert(pw_wine_process_close(getpid()+1) == -1 && errno == ESRCH);
    assert(pw_wine_thread_kill(getpid()+1,77,SIGUSR1) == -1 && errno == ESRCH);
    sampler_case();
    /* Last: the hook cannot be unset. */
    start_hook_case();
    puts("thread registry: priority calls, exit races, the sampler and the thread start hook passed");
}
