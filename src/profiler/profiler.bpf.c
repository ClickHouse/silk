#include <vmlinux.h>

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

const volatile u32 target_tgid = 0;
const volatile u8 kernel_stacks = 0;

struct
{
    __uint(type, BPF_MAP_TYPE_STACK_TRACE);
    __uint(key_size, sizeof(u32));
    __uint(value_size, 127 * sizeof(u64));
    __uint(max_entries, 65536);
} stack_map SEC(".maps");

// on-CPU: sample count keyed by combined stack ID
// key = ((u32)userSid << 32) | (u32)kernelSid; (u32)-1 means absent
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

char LICENSE[] SEC("license") = "GPL";
