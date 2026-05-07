#include <vmlinux.h>

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/usdt.bpf.h>

const volatile u32 target_tgid = 0;
const volatile u8 kernel_stacks = 0;

struct
{
    __uint(type, BPF_MAP_TYPE_STACK_TRACE);
    __uint(key_size, sizeof(u32));
    __uint(value_size, 127 * sizeof(u64));
    __uint(max_entries, 65536);
} stack_map SEC(".maps");

// on-CPU: sample count keyed by combined stack key - see makeStackKey
struct
{
    __uint(type, BPF_MAP_TYPE_PERCPU_HASH);
    __uint(max_entries, 65536);
    __type(key, u64);
    __type(value, u64);
} oncpu SEC(".maps");

// off-CPU: total nanoseconds blocked, same key encoding as oncpu
struct
{
    __uint(type, BPF_MAP_TYPE_PERCPU_HASH);
    __uint(max_entries, 65536);
    __type(key, u64);
    __type(value, u64);
} offcpu SEC(".maps");

// per-TID timestamp when thread went off CPU
struct
{
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u32);
    __type(value, u64);
} sleep_start SEC(".maps");

// per-TID combined stack key captured at sleep point
struct
{
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u32);
    __type(value, u64);
} sleep_stack SEC(".maps");

// Per-fiber lifecycle state, keyed by Fiber * (passed as arg0 of every
// silk:fiber_* USDT probe). Each entry tracks the fiber's current phase and
// when it entered that phase. Inserted at fiber_start, removed at fiber_stop.
//
// Phases:
//   1 = WAITING -- between fiber_start and fiber_schedule, or between
//                  fiber_exit and the next fiber_schedule (blocked on IO/sync)
//   2 = WAKEUP  -- between fiber_schedule and fiber_enter (on a ready queue)
//   3 = RUNNING -- between fiber_enter and fiber_exit (executing)
#define PHASE_WAITING 1
#define PHASE_WAKEUP 2
#define PHASE_RUNNING 3

struct fiber_state_t
{
    u64 last_ts;
    u8 last_phase;
};

struct
{
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, u64);
    __type(value, struct fiber_state_t);
} fiber_state SEC(".maps");

// Latency histogram, per-CPU to avoid contention on the probe hot path.
// Key encoding: bits 0..7 bucket, 8..15 phase, 16..23 fiber category.
// Bucket = clamped log2(ns); covers 1ns..2^31 ns (~2.1s).
struct
{
    __uint(type, BPF_MAP_TYPE_PERCPU_HASH);
    __uint(max_entries, 4096);
    __type(key, u64);
    __type(value, u64);
} latency_hist SEC(".maps");

static __always_inline u64 makeStackKey(s32 userSid, s32 kernelSid)
{
    return ((u64)(u32)userSid << 32) | (u32)kernelSid;
}

static __always_inline void incrementMap(void * map, u64 key, u64 delta)
{
    u64 zero = 0;
    bpf_map_update_elem(map, &key, &zero, BPF_NOEXIST);
    u64 * total = bpf_map_lookup_elem(map, &key);
    if (total)
    {
        *total += delta;
    }
}

// Clamped log2 in 0..31. Used to bucket nanosecond intervals.
static __always_inline u8 log2Bucket(u64 ns)
{
    if (ns == 0)
    {
        return 0;
    }
    u8 b = 0;
    if (ns >> 32)
    {
        b += 32;
        ns >>= 32;
    }
    if (ns >> 16)
    {
        b += 16;
        ns >>= 16;
    }
    if (ns >> 8)
    {
        b += 8;
        ns >>= 8;
    }
    if (ns >> 4)
    {
        b += 4;
        ns >>= 4;
    }
    if (ns >> 2)
    {
        b += 2;
        ns >>= 2;
    }
    if (ns >> 1)
    {
        b += 1;
    }
    if (b > 31)
        b = 31;
    return b;
}

static __always_inline void bumpLatency(u64 fiber_id, u8 phase, u64 ns)
{
    u8 category = (u8)(fiber_id >> 56);
    u8 bucket = log2Bucket(ns);
    u64 key = ((u64)category << 16) | ((u64)phase << 8) | bucket;
    incrementMap(&latency_hist, key, 1);
}

SEC("perf_event")
int on_cpu_sample(struct bpf_perf_event_data * ctx)
{
    u64 pidtgid = bpf_get_current_pid_tgid();
    u32 tgid = (u32)(pidtgid >> 32);

    if (target_tgid && tgid != target_tgid)
    {
        return 0;
    }

    s32 userSid = bpf_get_stackid(ctx, &stack_map, BPF_F_USER_STACK | BPF_F_REUSE_STACKID);
    if (userSid < 0)
    {
        return 0;
    }

    s32 kernelSid = -1;
    if (kernel_stacks)
    {
        kernelSid = bpf_get_stackid(ctx, &stack_map, BPF_F_REUSE_STACKID);
        if (kernelSid < 0)
        {
            kernelSid = -1;
        }
    }

    incrementMap(&oncpu, makeStackKey(userSid, kernelSid), 1);
    return 0;
}

SEC("tp_btf/sched_switch")
int BPF_PROG(on_sched_switch, bool preempt, struct task_struct * prev, struct task_struct * next)
{
    u32 prev_tgid = BPF_CORE_READ(prev, tgid);
    u32 prev_tid = BPF_CORE_READ(prev, pid);
    u32 next_tgid = BPF_CORE_READ(next, tgid);
    u32 next_tid = BPF_CORE_READ(next, pid);

    u64 now = bpf_ktime_get_ns();

    // Thread going off CPU: record timestamp and stack if voluntarily sleeping.
    // preempt == true means TASK_RUNNING (preempted); it will appear in on-CPU samples.
    if (!preempt && (!target_tgid || prev_tgid == target_tgid))
    {
        s32 userSid = bpf_get_stackid(ctx, &stack_map, BPF_F_USER_STACK | BPF_F_REUSE_STACKID);
        if (userSid < 0)
        {
            userSid = -1;
        }
        s32 kernelSid = -1;
        if (kernel_stacks)
        {
            kernelSid = bpf_get_stackid(ctx, &stack_map, BPF_F_REUSE_STACKID);
            if (kernelSid < 0)
            {
                kernelSid = -1;
            }
        }

        u64 key = makeStackKey(userSid, kernelSid);
        if (bpf_map_update_elem(&sleep_start, &prev_tid, &now, BPF_ANY) == 0)
        {
            bpf_map_update_elem(&sleep_stack, &prev_tid, &key, BPF_ANY);
        }
    }

    // Thread coming back on CPU: attribute elapsed off-CPU time to its sleep stack.
    if (!target_tgid || next_tgid == target_tgid)
    {
        u64 * start = bpf_map_lookup_elem(&sleep_start, &next_tid);
        if (start)
        {
            u64 delta = now - *start;
            bpf_map_delete_elem(&sleep_start, &next_tid);

            u64 * keyPtr = bpf_map_lookup_elem(&sleep_stack, &next_tid);
            if (keyPtr)
            {
                u64 key = *keyPtr;
                bpf_map_delete_elem(&sleep_stack, &next_tid);
                incrementMap(&offcpu, key, delta);
            }
        }
    }

    return 0;
}

// silk:fiber_start - fiber created. Seed the per-fiber state in WAITING phase
// (interval until the first fiber_schedule is the initial dispatch latency).
SEC("usdt")
int BPF_USDT(on_fiber_start, void * fiber_ptr, u64 fiber_id)
{
    u64 fkey = (u64)fiber_ptr;
    struct fiber_state_t state = {.last_ts = bpf_ktime_get_ns(), .last_phase = PHASE_WAITING};
    bpf_map_update_elem(&fiber_state, &fkey, &state, BPF_ANY);
    return 0;
}

// silk:fiber_schedule - fiber became READY. Close the WAITING interval and
// open a WAKEUP interval. Tolerates the start-of-trace case (no prior state)
// by silently skipping the histogram bump.
SEC("usdt")
int BPF_USDT(on_fiber_schedule, void * fiber_ptr, u64 fiber_id)
{
    u64 fkey = (u64)fiber_ptr;
    u64 now = bpf_ktime_get_ns();
    struct fiber_state_t * state = bpf_map_lookup_elem(&fiber_state, &fkey);
    if (state)
    {
        if (state->last_phase == PHASE_WAITING)
        {
            bumpLatency(fiber_id, PHASE_WAITING, now - state->last_ts);
        }
        state->last_ts = now;
        state->last_phase = PHASE_WAKEUP;
    }
    else
    {
        // Profiler attached mid-flight: seed state.
        struct fiber_state_t fresh = {.last_ts = now, .last_phase = PHASE_WAKEUP};
        bpf_map_update_elem(&fiber_state, &fkey, &fresh, BPF_ANY);
    }
    return 0;
}

// silk:fiber_enter - fiber starts running. Close WAKEUP interval, open RUNNING.
SEC("usdt")
int BPF_USDT(on_fiber_enter, void * fiber_ptr, u64 fiber_id)
{
    u64 fkey = (u64)fiber_ptr;
    u64 now = bpf_ktime_get_ns();
    struct fiber_state_t * state = bpf_map_lookup_elem(&fiber_state, &fkey);
    if (state)
    {
        if (state->last_phase == PHASE_WAKEUP)
        {
            bumpLatency(fiber_id, PHASE_WAKEUP, now - state->last_ts);
        }
        state->last_ts = now;
        state->last_phase = PHASE_RUNNING;
    }
    else
    {
        struct fiber_state_t fresh = {.last_ts = now, .last_phase = PHASE_RUNNING};
        bpf_map_update_elem(&fiber_state, &fkey, &fresh, BPF_ANY);
    }
    return 0;
}

// silk:fiber_exit - fiber stops running (either suspending or about to stop).
// Close RUNNING interval; reopen WAITING (until the next schedule, or until
// fiber_stop tears the entry down).
SEC("usdt")
int BPF_USDT(on_fiber_exit, void * fiber_ptr, u64 fiber_id)
{
    u64 fkey = (u64)fiber_ptr;
    u64 now = bpf_ktime_get_ns();
    struct fiber_state_t * state = bpf_map_lookup_elem(&fiber_state, &fkey);
    if (state)
    {
        if (state->last_phase == PHASE_RUNNING)
        {
            bumpLatency(fiber_id, PHASE_RUNNING, now - state->last_ts);
        }
        state->last_ts = now;
        state->last_phase = PHASE_WAITING;
    }
    return 0;
}

// silk:fiber_stop - fiber destroyed. Drop its state entry.
SEC("usdt")
int BPF_USDT(on_fiber_stop, void * fiber_ptr, u64 fiber_id)
{
    u64 fkey = (u64)fiber_ptr;
    bpf_map_delete_elem(&fiber_state, &fkey);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
