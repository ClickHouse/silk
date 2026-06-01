"""
fiber.py -- GDB commands for debugging the fiber scheduler.

Usage: (gdb) source /src/gdb/fiber.py

Commands:
  fiber-list [--proxy]            List all known fibers
  fiber-savecontext               Save current thread and registers
  fiber-restorecontext            Restore the previously saved context
  fiber-switchcontext <Fiber*>    Switch to a SUSPENDED/READY fiber's context
"""

import gdb
import struct
import os

# ── Architecture ──────────────────────────────────────────────────────────────


def _arch():
    name = gdb.selected_frame().architecture().name()
    if "x86-64" in name:
        return "x86_64"
    if "aarch64" in name:
        return "aarch64"
    raise RuntimeError(f"unsupported architecture: {name}")


# Boost.Context callee-saved frame layout.
# Each entry is (gdb_register_name, byte_offset_from_fctx).
# fctx is the saved SP/RSP value stored in Fiber::fiberContext.
#
# Offsets verified from Boost.Context ASM sources:
#   x86-64: https://github.com/boostorg/context/blob/develop/src/asm/jump_x86_64_sysv_elf_gas.S
#   arm64:  https://github.com/boostorg/context/blob/develop/src/asm/jump_arm64_aapcs_elf_gas.S
_FRAME_REGS = {
    "x86_64": [
        # [0x00] mxcsr + x87 cw  -- not a GP register, skip
        # [0x08] stack guard     -- not a GP register, skip
        ("r12", 0x10),
        ("r13", 0x18),
        ("r14", 0x20),
        ("r15", 0x28),
        ("rbx", 0x30),
        ("rbp", 0x38),
        ("rip", 0x40),
    ],
    "aarch64": [
        # [0x00..0x38] d8-d15  -- FP regs, skip for basic stack inspection
        ("x19", 0x40),
        ("x20", 0x48),
        ("x21", 0x50),
        ("x22", 0x58),
        ("x23", 0x60),
        ("x24", 0x68),
        ("x25", 0x70),
        ("x26", 0x78),
        ("x27", 0x80),
        ("x28", 0x88),
        ("x29", 0x90),  # frame pointer
        ("x30", 0x98),  # link register
        # [0xa0] pc = copy of lr, set separately via _PC_OFFSET
    ],
}
_FRAME_SIZE = {"x86_64": 0x48, "aarch64": 0xB0}
_SP_REG = {"x86_64": "rsp", "aarch64": "sp"}
_PC_REG = {"x86_64": "rip", "aarch64": "pc"}
_PC_OFFSET = {"x86_64": 0x40, "aarch64": 0xA0}
_FP_REG = {"x86_64": "rbp", "aarch64": "x29"}
_FP_OFFSET = {"x86_64": 0x38, "aarch64": 0x90}

# All registers to save/restore per architecture.
_ALL_REGS = {
    "x86_64": [
        "rax",
        "rbx",
        "rcx",
        "rdx",
        "rsi",
        "rdi",
        "rbp",
        "rsp",
        "r8",
        "r9",
        "r10",
        "r11",
        "r12",
        "r13",
        "r14",
        "r15",
        "rip",
        "eflags",
    ],
    "aarch64": [f"x{i}" for i in range(31)] + ["sp", "pc", "pstate"],
}

# ── Low-level memory helpers ──────────────────────────────────────────────────


def _read(addr, n):
    """Read n bytes from the inferior at addr."""
    return bytes(gdb.selected_inferior().read_memory(addr, n))


def _ptr(addr):
    """Read a 64-bit pointer from addr."""
    return struct.unpack_from("<Q", _read(addr, 8))[0]


def _u64(addr):
    """Read a 64-bit unsigned integer from addr."""
    return struct.unpack_from("<Q", _read(addr, 8))[0]


def _field_offset(type_val, name):
    """Return byte offset of @p name within @p type_val, searching recursively
    into anonymous structs/unions (unnamed fields)."""
    for field in type_val.fields():
        if field.name == name:
            return field.bitpos // 8
        if not field.name:
            try:
                inner = _field_offset(field.type, name)
                if inner is not None:
                    return field.bitpos // 8 + inner
            except Exception:
                pass
    return None


# ── stdlib helpers ─────────────────────────────────────────────────────────────


def _unique_ptr(val):
    """Return the managed pointer (as int) from a std::unique_ptr gdb.Value.

    Uses the empty-base-optimization guarantee: with default_delete (an empty
    type) the managed pointer is always the first word of the unique_ptr object.
    Layout-stable across libstdc++ and libc++ versions.
    """
    return _ptr(int(val.address))


def _atomic_load(val):
    """Return the stored value of a std::atomic<T> gdb.Value.

    For lock-free atomics, T is stored at offset 0 of the atomic object.
    Falls back to raw memory when the internal field (which may live in a
    base class such as __atomic_base<T>) is not directly accessible via GDB.
    """
    for field in ("_M_i", "__val_", "_M_b._M_i"):
        try:
            return val[field]
        except gdb.error:
            pass
    # Fallback: read the first 8 bytes at the atomic's address.
    # Correct for all lock-free scalar and pointer atomics.
    return gdb.Value(_u64(int(val.address)))


# ── Fiber helpers ─────────────────────────────────────────────────────────────

_FIBER_STATES = {
    0: "SUSPENDED",
    1: "READY",
    2: "RUNNING",
    3: "SUSPEND_REQUESTED",
    4: "STOPPED",
}


def _fiber_val(ptr):
    """Cast an integer address to a Fiber& gdb.Value."""
    t = gdb.lookup_type("silk::Fiber").pointer()
    return gdb.Value(ptr).cast(t).dereference()


def _fiber_state_str(val):
    try:
        raw = int(_atomic_load(val["state"]))
        return _FIBER_STATES.get(raw, f"?{raw}")
    except gdb.error:
        return "?"


def _resolve_fn(addr):
    """Resolve an address to 'symbol at file:line', or '0xaddr' if unknown."""
    if not addr:
        return "null"
    sym = f"0x{addr:x}"
    try:
        out = gdb.execute(f"info symbol {addr}", to_string=True).strip()
        if out and "No symbol" not in out:
            sym = out.split("\n")[0].split(" in section")[0].strip()
    except gdb.error:
        pass
    try:
        sal = gdb.find_pc_line(addr)
        if sal.symtab and sal.line:
            fname = os.path.basename(sal.symtab.filename)
            sym += f" at {fname}:{sal.line}"
    except gdb.error:
        pass
    return sym


# Symbols that belong to the scheduler's context-switch internals.
# Used to skip past them when walking the frame chain of a suspended fiber.
_SCHEDULER_SYMS = ("jump_fcontext", "switchToThread", "FiberScheduler", "fiber.cpp")


def _suspension_site(fctx):
    """Walk the frame pointer chain from a fiber's fctx and return the first
    symbol outside the scheduler internals, or None if not found.
    """
    arch = _arch()
    fp = _ptr(fctx + _FP_OFFSET[arch])
    for _ in range(12):
        if not fp or fp < 0x1000:
            break
        sym = _resolve_fn(_ptr(fp + 8))
        if not any(s in sym for s in _SCHEDULER_SYMS):
            return sym
        fp = _ptr(fp)
    return None


def _format_fiber(ptr):
    """Format a single fiber row for fiber-list output."""
    try:
        f = _fiber_val(ptr)
        state = _fiber_state_str(f)
        fmain = int(f["fiberMain"])
        sym = _resolve_fn(fmain)
        line = f"  0x{ptr:016x}  {state:<9}  0x{fmain:016x}  {sym}"
        if state in ("SUSPENDED", "SUSPEND_REQUESTED"):
            fctx = int(f["fiberContext"])
            site = _suspension_site(fctx) if fctx else None
            if site:
                line += f"\n{'':56}  suspended at {site}"
        return line
    except gdb.error as e:
        return f"  0x{ptr:016x}  <error: {e}>"


# ── List walker ───────────────────────────────────────────────────────────────


def _walk_list(val, entry_offset):
    """
    Walk a List<T, nodePtr> gdb.Value, yielding T* integer addresses.

    List<T,...> contains a single boost::intrusive::list member at offset 0.
    With constant_time_size<false>, that object begins with the circular
    sentinel node {m_next, m_prev}.  Each ListEntry hook has the same layout.
    We walk m_next pointers (at offset 0 of each hook) until we return to the
    sentinel.

    @param list_val     gdb.Value of the List<T,...> object (must be an lvalue).
    @param entry_offset byte offset of the ListEntry hook within T.
    """
    sentinel_addr = int(val.address)
    hook_ptr = _ptr(sentinel_addr)
    seen = set()
    while hook_ptr != sentinel_addr and hook_ptr not in seen:
        seen.add(hook_ptr)
        yield hook_ptr - entry_offset
        hook_ptr = _ptr(hook_ptr)  # hook.m_next at offset 0


def _walk_suspended_list(val):
    """Walk ProcessorState::suspendedList, yielding Fiber* integer addresses."""
    try:
        fiber_type = gdb.lookup_type("silk::Fiber")
        entry_offset = _field_offset(fiber_type, "suspendedEntry")
        if entry_offset is None:
            return
        yield from _walk_list(val["suspendedList"], entry_offset)
    except Exception:
        pass


# ── Queue walkers ─────────────────────────────────────────────────────────────


def _walk_queue(val):
    """
    Walk a QueueBase, yielding Fiber* integer addresses.

    QueueBase layout:
      offset  0: atomic<TaggedPtr> head   (TaggedPtr = {QueueNode* ptr, uint64_t tag})
      offset 16: atomic<TaggedPtr> tail

    QueueNode layout:
      offset  0: StackEntry stackEntry           (atomic<StackEntry*> next -- LFS node, unused)
      offset  8: atomic<QueueNode*> next         (M&S next pointer)
      offset 16: atomic<void*> value             (the enqueued Fiber*)

    head.ptr is the sentinel node; real items start at sentinel->next.
    """
    base_addr = int(val.address)
    sentinel = _ptr(base_addr)  # head.ptr = sentinel QueueNode*
    if not sentinel:
        return

    node = _ptr(sentinel + 8)  # sentinel->next (QueueNode::next at offset 8)
    seen = set()
    while node and node not in seen:
        seen.add(node)
        value = _ptr(node + 16)  # QueueNode::value at offset 16
        if value:
            yield value
        node = _ptr(node + 8)  # next QueueNode


def _walk_bounded_queue(val):
    """
    Walk a BoundedQueue<Fiber*>, yielding Fiber* integer addresses.

    BoundedQueue<Fiber*> layout (Slot is private; we use raw memory with stride 64):
      offset   0: uint64_t mask
      offset   8: unique_ptr<Slot[]> slots   (EBO: managed pointer is first word)
      offset  64: atomic<uint64_t> enqueuePos   (alignas(64))
      offset 128: atomic<uint64_t> dequeuePos   (alignas(64))

    Slot layout (alignas(64), stride 64 bytes):
      offset 0: atomic<uint64_t> sequence
      offset 8: Fiber* value

    A slot at ring position pos is ready for dequeue when sequence == pos + 1.
    """
    mask = int(val["mask"])
    enqueue_pos = int(_atomic_load(val["enqueuePos"]))
    dequeue_pos = int(_atomic_load(val["dequeuePos"]))
    slots_base = _unique_ptr(val["slots"])

    # Capacity sanity: at most mask+1 items can be in flight.
    capacity = mask + 1
    num_items = min(enqueue_pos - dequeue_pos, capacity)

    for i in range(num_items):
        pos = dequeue_pos + i
        slot_addr = slots_base + (pos & mask) * 64  # stride = CACHELINE_SIZE
        seq = _u64(slot_addr)
        if seq == pos + 1:  # slot has a valid value
            value = _ptr(slot_addr + 8)
            if value:
                yield value


# ── Thread fiber walker ───────────────────────────────────────────────────────


def _walk_thread_fibers(show_proxy=False):
    """
    Yield (gdb.InferiorThread, Fiber* int) for every OS thread with a non-null
    threadFiber TLS value (covers both scheduler and worker threads).
    Proxy fibers (isProxyFiber=true) are skipped unless show_proxy is True.
    """
    orig = gdb.selected_thread()
    try:
        for thread in gdb.inferiors()[0].threads():
            thread.switch()
            try:
                ptr = int(gdb.parse_and_eval("'fiber.cpp'::silk::threadFiber"))
                if ptr:
                    if show_proxy or not bool(_fiber_val(ptr)["isProxyFiber"]):
                        yield (thread, ptr)
            except gdb.error:
                pass
    finally:
        if orig and orig.is_valid():
            orig.switch()


# ── Fiber enumeration ─────────────────────────────────────────────────────────


def _get_scheduler():
    """Return the dereferenced FiberScheduler::scheduler gdb.Value."""
    ptr = gdb.parse_and_eval("silk::FiberScheduler::scheduler")
    if int(ptr) == 0:
        raise RuntimeError(
            "FiberScheduler::scheduler is null (scheduler not initialized)"
        )
    return ptr.dereference()


def _enumerate_fibers(sched, seen, show_proxy=False):
    """
    Yield Fiber* int for:
      - per-CPU readyQueue and suspendedList
      - shared readyQueue
      - thread TLS (covers RUNNING fibers on both scheduler and worker threads)

    Updates seen to deduplicate across all sources.
    """

    def emit(ptr):
        if ptr and ptr not in seen:
            seen.add(ptr)
            return ptr
        return None

    proc_count = int(sched["processorCount"])

    # Per-CPU ready queues and suspended lists
    ps_type = gdb.lookup_type("silk::FiberScheduler::ProcessorState").pointer()
    proc_states = gdb.Value(_unique_ptr(sched["processorState"])).cast(ps_type)
    for i in range(proc_count):
        proc = proc_states[i]
        for ptr in _walk_bounded_queue(proc["readyQueue"]):
            ptr = emit(ptr)
            if ptr:
                yield ptr
        for ptr in _walk_suspended_list(proc):
            ptr = emit(ptr)
            if ptr:
                yield ptr

    # Shared ready queue (thread-mode fibers and per-CPU overflow)
    for ptr in _walk_queue(sched["readyQueue"]):
        ptr = emit(ptr)
        if ptr:
            yield ptr

    # Thread TLS: covers RUNNING fibers on scheduler and worker threads
    for thread, ptr in _walk_thread_fibers(show_proxy=show_proxy):
        ptr = emit(ptr)
        if ptr:
            yield ptr


# ── Saved context ─────────────────────────────────────────────────────────────

_saved_ctx = None  # {"arch": str, "thread_num": int, "regs": {name: int}}


def _get_reg(name):
    return int(gdb.parse_and_eval(f"${name}"))


def _set_reg(name, value):
    gdb.execute(f"set ${name} = {value}", to_string=True)


# ── Commands ──────────────────────────────────────────────────────────────────


class FiberList(gdb.Command):
    """fiber-list [--proxy]

    List all fibers visible in the scheduler's data structures.

    --proxy  Also show proxy fibers (created by getCurrentFiber() on non-fiber
             threads; fiberMain is null since they represent OS threads, not
             fiber workloads).

    Columns: FIBER* (pass to fiber-switchcontext)  STATE  FIBERMAIN address+symbol
    Use 'p *(Fiber*)0xADDR' to inspect all fields of a specific fiber.
    """

    def __init__(self):
        super().__init__("fiber-list", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        show_proxy = "--proxy" in arg

        try:
            sched = _get_scheduler()
        except (RuntimeError, gdb.error) as e:
            print(f"fiber-list: {e}")
            return

        seen = set()

        print(f"  {'FIBER*':<18} {'STATE':<9}  {'FIBERMAIN'}")
        print(f"  {'-'*18} {'-'*9}  {'-'*60}")

        for ptr in _enumerate_fibers(sched, seen, show_proxy=show_proxy):
            print(_format_fiber(ptr))


class FiberSaveContext(gdb.Command):
    """fiber-savecontext

    Save the current GDB thread number and all registers to an in-memory snapshot.
    Must be called before fiber-switchcontext so that fiber-restorecontext can
    return here.

    A new save overwrites the previous one (prints a warning).
    """

    def __init__(self):
        super().__init__("fiber-savecontext", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        global _saved_ctx
        if _saved_ctx is not None:
            print("fiber-savecontext: warning: overwriting previous saved context")

        arch = _arch()
        thread_num = gdb.selected_thread().num
        regs = {r: _get_reg(r) for r in _ALL_REGS[arch]}
        _saved_ctx = {"arch": arch, "thread_num": thread_num, "regs": regs}

        pc_reg = _PC_REG[arch]
        print(
            f"fiber-savecontext: saved (thread {thread_num}, arch={arch}, "
            f"{pc_reg}=0x{regs[pc_reg]:x})"
        )


class FiberRestoreContext(gdb.Command):
    """fiber-restorecontext

    Restore the thread and registers saved by fiber-savecontext.
    """

    def __init__(self):
        super().__init__("fiber-restorecontext", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        global _saved_ctx
        if _saved_ctx is None:
            print(
                "fiber-restorecontext: no saved context (run fiber-savecontext first)"
            )
            return

        # Switch back to the original thread.
        target_num = _saved_ctx["thread_num"]
        for thread in gdb.inferiors()[0].threads():
            if thread.num == target_num:
                thread.switch()
                break
        else:
            print(
                f"fiber-restorecontext: warning: thread {target_num} no longer exists"
            )

        # Restore all registers.
        for reg, value in _saved_ctx["regs"].items():
            _set_reg(reg, value)

        pc_reg = _PC_REG[_saved_ctx["arch"]]
        print(
            f"fiber-restorecontext: restored (thread {target_num}, "
            f"{pc_reg}=0x{_saved_ctx['regs'][pc_reg]:x})"
        )
        gdb.execute("frame 0")


class FiberSwitchContext(gdb.Command):
    """fiber-switchcontext <Fiber*>

    Switch the current GDB register state to the saved context of a SUSPENDED or
    READY fiber.  The fiber is identified by its Fiber* address (as shown by
    fiber-list).

    After this command, 'bt' shows the fiber's call stack.

    For RUNNING fibers (on a scheduler or worker thread) use 'thread N' instead --
    no register manipulation is needed.

    Call fiber-savecontext first so that fiber-restorecontext can return here.
    """

    def __init__(self):
        super().__init__("fiber-switchcontext", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        arg = arg.strip()
        if not arg:
            print("usage: fiber-switchcontext <Fiber*>")
            return
        if _saved_ctx is None:
            print(
                "fiber-switchcontext: warning: no saved context -- "
                "run fiber-savecontext first or fiber-restorecontext will not work"
            )

        try:
            ptr = int(gdb.parse_and_eval(arg))
        except gdb.error as e:
            print(f"fiber-switchcontext: cannot parse address: {e}")
            return

        try:
            fiber = _fiber_val(ptr)
        except gdb.error as e:
            print(f"fiber-switchcontext: cannot read Fiber at 0x{ptr:x}: {e}")
            return

        state = _fiber_state_str(fiber)
        if state not in ("SUSPENDED", "SUSPEND_REQUESTED", "READY"):
            print(
                f"fiber-switchcontext: fiber 0x{ptr:x} is {state} -- "
                f"use 'thread N' for RUNNING fibers"
            )
            return

        fctx = int(fiber["fiberContext"])
        if not fctx:
            print(f"fiber-switchcontext: fiber 0x{ptr:x} has null fiberContext")
            return

        arch = _arch()
        frame_regs = _FRAME_REGS[arch]
        frame_size = _FRAME_SIZE[arch]
        sp_reg = _SP_REG[arch]
        pc_reg = _PC_REG[arch]
        pc_offset = _PC_OFFSET[arch]

        # Read the Boost.Context frame from the fiber's saved stack.
        frame_data = _read(fctx, frame_size)

        # Restore callee-saved registers.
        for reg, offset in frame_regs:
            value = struct.unpack_from("<Q", frame_data, offset)[0]
            _set_reg(reg, value)

        # Restore SP to just above the frame (the fiber's live stack region).
        _set_reg(sp_reg, fctx + frame_size)

        # Set PC.  On aarch64, x30 (lr) holds the resume address; also set pc.
        pc_value = struct.unpack_from("<Q", frame_data, pc_offset)[0]
        if arch == "aarch64":
            _set_reg("pc", pc_value)

        fmain = int(fiber["fiberMain"])
        print(
            f"fiber-switchcontext: switched to 0x{ptr:x} "
            f"(state={state}, {sp_reg}=0x{fctx + frame_size:x}, "
            f"{pc_reg}=0x{pc_value:x}, fiberMain={_resolve_fn(fmain)})"
        )
        gdb.execute("frame 0")


# ── Register commands ─────────────────────────────────────────────────────────

FiberList()
FiberSaveContext()
FiberSwitchContext()
FiberRestoreContext()

print("fiber.py loaded -- commands:")
print("  fiber-list [--proxy]")
print("  fiber-savecontext")
print("  fiber-restorecontext")
print("  fiber-switchcontext <Fiber*>")
