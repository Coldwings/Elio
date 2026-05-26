#!/usr/bin/env python3
"""
Elio Coroutine Library - LLDB Python Extension

This script provides LLDB commands for debugging Elio coroutines (vthreads).
It finds coroutine frames by traversing the scheduler's worker queues.

Usage:
    Preferred: command script import /path/to/elio_lldb.py
    (This file remains the implementation module.)

Commands:
    elio list              - List all vthreads from worker queues
    elio bt [id]           - Show backtrace for vthread(s)
    elio info <id>         - Show detailed info for a vthread
    elio workers           - Show worker thread information
    elio stats             - Show scheduler statistics

Works with both live processes and coredumps.
"""

import lldb

# Magic number for frame validation
FRAME_MAGIC = 0x454C494F46524D45  # "ELIOFRME"

# Coroutine states
COROUTINE_STATES = {
    0: "created",
    1: "running",
    2: "suspended",
    3: "completed",
    4: "failed"
}


def read_cstring(process, addr):
    """Read a null-terminated string from memory."""
    if addr == 0:
        return None
    
    error = lldb.SBError()
    result = []
    max_len = 1024
    
    for i in range(max_len):
        byte = process.ReadUnsignedFromMemory(addr + i, 1, error)
        if error.Fail() or byte == 0:
            break
        result.append(chr(byte))
    
    return ''.join(result) if result else None


def read_uint64(process, addr):
    """Read a uint64 from memory."""
    error = lldb.SBError()
    return process.ReadUnsignedFromMemory(addr, 8, error)


def read_uint32(process, addr):
    """Read a uint32 from memory."""
    error = lldb.SBError()
    return process.ReadUnsignedFromMemory(addr, 4, error)


def read_uint8(process, addr):
    """Read a uint8 from memory."""
    error = lldb.SBError()
    return process.ReadUnsignedFromMemory(addr, 1, error)


def read_pointer(process, addr):
    """Read a pointer from memory."""
    error = lldb.SBError()
    ptr_size = process.GetAddressByteSize()
    return process.ReadUnsignedFromMemory(addr, ptr_size, error)


def get_scheduler(target, process):
    """Find the current scheduler."""
    # Try to find scheduler::current()
    try:
        # Look for the current_scheduler_ thread-local or global variable
        symbols = target.FindGlobalVariables("elio::runtime::current_scheduler_", 1)
        if symbols.GetSize() > 0:
            sched_var = symbols.GetValueAtIndex(0)
            if sched_var.IsValid():
                sched_ptr = sched_var.GetValueAsUnsigned(0)
                if sched_ptr != 0:
                    return sched_ptr
    except:
        pass
    
    # Try alternative symbol names
    for name in ["current_scheduler_", "_ZN4elio7runtime18current_scheduler_E"]:
        try:
            symbols = target.FindGlobalVariables(name, 10)
            for i in range(symbols.GetSize()):
                sched_var = symbols.GetValueAtIndex(i)
                if sched_var.IsValid():
                    sched_ptr = sched_var.GetValueAsUnsigned(0)
                    if sched_ptr != 0:
                        return sched_ptr
        except:
            pass
    
    return None


def get_frame_from_handle(process, handle_addr):
    """Extract promise_base info from a coroutine handle address.

    Note: Debug metadata fields (location, state, worker_id, id) are only
    available when ELIO_ENABLE_DEBUG_METADATA=1. When disabled, these fields
    will return default/None values.
    """
    if handle_addr == 0:
        return None

    try:
        ptr_size = process.GetAddressByteSize()

        # Coroutine frame layout (typical):
        # - resume function pointer
        # - destroy function pointer
        # - promise object

        # Promise is typically at offset 2*ptr_size (after resume and destroy)
        promise_addr = handle_addr + 2 * ptr_size

        # Read magic to validate
        magic = read_uint64(process, promise_addr)
        if magic != FRAME_MAGIC:
            return None

        # Read promise_base fields (layout varies based on ELIO_ENABLE_DEBUG_METADATA)
        # Base layout (always present): magic(8) + parent(8) + exception(16)
        # Debug layout (when enabled): + debug_location(24) + state(1) + pad(3) + worker_id(4) + debug_id(8)
        # Affinity: size_t (8 bytes on 64-bit)

        # Read parent pointer (always present)
        parent = read_pointer(process, promise_addr + 8)

        # Try to detect if debug metadata is present
        # We'll try to read the debug fields and gracefully handle failures

        # First, try to read debug metadata (assume enabled by default)
        has_debug_metadata = True
        debug_location_offset = 8 + ptr_size + 16  # magic + parent + exception_ptr

        try:
            # Try reading debug_location.file pointer
            file_ptr = read_pointer(process, promise_addr + debug_location_offset)
            func_ptr = read_pointer(process, promise_addr + debug_location_offset + ptr_size)
            line = read_uint32(process, promise_addr + debug_location_offset + 2 * ptr_size)

            # state at debug_location_offset + 24
            state_offset = debug_location_offset + 2 * ptr_size + 4
            state = read_uint8(process, promise_addr + state_offset)

            # worker_id at state_offset + 4 (after 3 bytes padding)
            worker_id = read_uint32(process, promise_addr + state_offset + 4)

            # debug_id at worker_id + 4
            debug_id = read_uint64(process, promise_addr + state_offset + 8)
        except Exception as e:
            # Debug metadata not available - use defaults
            has_debug_metadata = False
            file_ptr = 0
            func_ptr = 0
            line = 0
            state = 1  # running
            worker_id = 0xFFFFFFFF  # Unknown
            debug_id = 0

        return {
            "id": debug_id,
            "state": COROUTINE_STATES.get(state, "unknown"),
            "worker_id": worker_id,
            "parent": parent,
            "file": read_cstring(process, file_ptr) if has_debug_metadata else None,
            "function": read_cstring(process, func_ptr) if has_debug_metadata else None,
            "line": line if has_debug_metadata else 0,
            "address": handle_addr,
            "promise_addr": promise_addr,
            "has_debug_metadata": has_debug_metadata
        }
    except Exception as e:
        return None


def walk_virtual_stack(process, handle_addr):
    """Walk the virtual stack from a coroutine handle."""
    stack = []
    visited = set()

    info = get_frame_from_handle(process, handle_addr)
    if info:
        stack.append(info)
        visited.add(handle_addr)

        # Walk parent chain
        parent = info["parent"]
        while parent != 0 and parent not in visited:
            visited.add(parent)
            # Parent is a promise_base*, need to find the frame address
            # For now just note we have a parent
            stack.append({"id": 0, "address": parent, "state": "parent",
                         "function": None, "file": None, "line": 0, "worker_id": 0xFFFFFFFF,
                         "has_debug_metadata": False})
            break

    return stack


def read_atomic_value(process, addr, size=8):
    """Read value from std::atomic (trying common layouts)."""
    error = lldb.SBError()
    
    # Try direct read first
    val = process.ReadUnsignedFromMemory(addr, size, error)
    if not error.Fail():
        return val
    
    return None


def iterate_chase_lev_deque(process, queue_addr, ptr_size):
    """Iterate over entries in a Chase-Lev deque.

    chase_lev_deque has ``alignas(64)`` on top_, bottom_, and buffer_, so they
    sit on consecutive 64-byte cache lines (offsets 0, 64, 128). buffer_ is an
    ``atomic<circular_buffer*>``; the circular_buffer object itself starts with
    capacity_, mask_, then a ``unique_ptr<atomic<T*>[]>`` payload.
    """
    try:
        top = read_uint64(process, queue_addr)
        bottom = read_uint64(process, queue_addr + 64)
        buffer_ptr = read_pointer(process, queue_addr + 128)

        if top is None or bottom is None or buffer_ptr == 0:
            return

        # circular_buffer layout (chase_lev_deque.hpp): capacity_, mask_,
        # then a unique_ptr<atomic<T*>[]> whose held pointer is the slot
        # array. Both members are size_t.
        capacity = read_uint64(process, buffer_ptr)
        slots_ptr = read_pointer(process, buffer_ptr + 2 * 8)

        if not capacity or slots_ptr == 0:
            return

        for i in range(top, bottom):
            try:
                idx = i % capacity
                entry_addr = slots_ptr + idx * ptr_size
                entry = read_pointer(process, entry_addr)
                if entry and entry != 0:
                    yield entry
            except Exception:
                pass
    except Exception:
        pass


# NOTE: MPSC inbox traversal is omitted on purpose; see iterate_worker_tasks.

# ---------------------------------------------------------------------------
# Known limitation: I/O-suspended coroutines are NOT enumerated.
#
# Coroutines awaiting an io_uring SQE are not in any worker's run-queue. Their
# coroutine handle is reachable only through ``op_state``s referenced by
# tagged ``user_data`` values inside the kernel-side io_uring CQ ring (see
# include/elio/io/io_uring_backend.hpp). The backend keeps a count
# (``pending_ops_``) but no walkable container of in-flight op_states, so
# iterating them from a debugger requires walking io_uring's mmap'd CQ ring
# — kernel-version-dependent and out of scope for this script. Production
# stacks dominated by I/O suspension will therefore report few or no tasks
# from ``elio list``; check ``elio workers`` for queue counts and combine
# with native ``thread backtrace all`` for full coverage.
# ---------------------------------------------------------------------------


def iterate_worker_tasks(process, worker_addr, ptr_size, target=None):
    """Iterate over all tasks in a worker's queues.

    worker_thread holds ``queue_`` (Chase-Lev deque) and ``inbox_`` (MPSC) as
    ``unique_ptr``s, so we read the held pointer at the field offset and then
    walk the heap-allocated container.
    """
    try:
        # worker_thread layout (worker_thread.hpp:226+):
        #   scheduler_ (ptr) -> 0
        #   worker_id_ (size_t) -> 8
        #   queue_ (unique_ptr<chase_lev_deque<void>>) -> 16
        #   inbox_ (unique_ptr<mpsc_queue<void>>)      -> 24
        queue_offset = ptr_size + ptr_size  # after scheduler_ + worker_id_
        if target is not None:
            queue_offset = _resolve_field_offset(
                target, "elio::runtime::worker_thread", "queue_", queue_offset)

        queue_ptr = read_pointer(process, worker_addr + queue_offset)
        if queue_ptr != 0:
            for addr in iterate_chase_lev_deque(process, queue_ptr, ptr_size):
                yield addr

        # inbox_ traversal is intentionally omitted: ``mpsc_queue`` uses an
        # array-of-slots layout with embedded sequence counters that is not
        # safe to walk lock-free from a debugger. Tasks visible there are
        # transient hand-offs to the worker; the deque view is sufficient
        # for an interactive snapshot.
    except Exception:
        pass


# scheduler::MAX_THREADS — kept in sync with include/elio/runtime/scheduler.hpp.
# Used as both a hard upper bound on the worker walk and as the inline size of
# the std::array<unique_ptr<worker_thread>, MAX_THREADS> ``workers_`` member.
SCHEDULER_MAX_THREADS = 256


def _get_field_offset(sbtype, field_name):
    """Find a field's byte offset on an SBType, or None if absent/invalid."""
    if not sbtype or not sbtype.IsValid():
        return None
    try:
        for i in range(sbtype.GetNumberOfFields()):
            f = sbtype.GetFieldAtIndex(i)
            if f.GetName() == field_name:
                return f.GetOffsetInBytes()
    except Exception:
        pass
    return None


def _resolve_field_offset(target, type_name, field_name, fallback):
    """Look up a field offset via the debugger's type system, with fallback.

    Falling back to a hard-coded offset matters for stripped binaries / partial
    debug info where the type lookup returns invalid. The fallbacks track the
    layout documented in scheduler.hpp / worker_thread.hpp.
    """
    try:
        t = target.FindFirstType(type_name)
        off = _get_field_offset(t, field_name)
        if off is not None:
            return off
    except Exception:
        pass
    return fallback


def iterate_all_tasks(target, process):
    """Iterate over all tasks across all workers.

    Layout invariants (see include/elio/runtime/scheduler.hpp):
      * ``workers_`` is ``std::array<std::unique_ptr<worker_thread>, MAX_THREADS>``
        stored inline in the scheduler — NOT a heap-allocated vector. Slot ``i``
        holds the unique_ptr's pointee directly at ``&workers_ + i * ptr_size``.
      * ``num_threads_`` is an ``alignas(64) std::atomic<size_t>`` that lives
        immediately after ``workers_``. Readers must clamp ``i`` to
        ``min(num_threads_, MAX_THREADS)`` to stay inside the array.

    Prior to PR #72 ``workers_`` was a heap-allocated vector and the walker
    chased ``vector::_M_start`` as a pointer. With std::array storage that
    offset now points into worker_thread #0's body, producing garbage worker
    pointers and segfaulting the inferior — hence the rewrite.
    """
    sched_addr = get_scheduler(target, process)
    if sched_addr is None:
        return

    ptr_size = process.GetAddressByteSize()

    # Default fallbacks track scheduler.hpp:645+ (post-PR #72 layout):
    #   workers_       at offset 0 (no earlier members; no vtable)
    #   num_threads_   alignas(64) right after the std::array storage
    default_workers_offset = 0
    default_num_threads_offset = ((SCHEDULER_MAX_THREADS * ptr_size) + 63) & ~63

    workers_offset = _resolve_field_offset(
        target, "elio::runtime::scheduler", "workers_", default_workers_offset)
    num_threads_offset = _resolve_field_offset(
        target, "elio::runtime::scheduler", "num_threads_", default_num_threads_offset)

    num_threads = read_uint64(process, sched_addr + num_threads_offset)
    if num_threads is None or num_threads == 0:
        return
    if num_threads > SCHEDULER_MAX_THREADS:
        num_threads = SCHEDULER_MAX_THREADS

    workers_base = sched_addr + workers_offset

    # worker_thread layout (worker_thread.hpp):
    #   scheduler_  (ptr,    8 bytes) at offset 0
    #   worker_id_  (size_t, 8 bytes) at offset 8
    worker_id_offset = _resolve_field_offset(
        target, "elio::runtime::worker_thread", "worker_id_", ptr_size)

    for i in range(num_threads):
        try:
            # Each std::array slot is a unique_ptr<worker_thread>. Both
            # libstdc++ and libc++ store the held pointer as the first (and
            # only) word, so reading ptr_size bytes recovers worker_ptr.
            slot_addr = workers_base + i * ptr_size
            worker_ptr = read_pointer(process, slot_addr)
            if worker_ptr == 0:
                continue

            worker_id = read_uint64(process, worker_ptr + worker_id_offset)

            for task_addr in iterate_worker_tasks(process, worker_ptr, ptr_size, target):
                yield task_addr, worker_id
        except Exception:
            continue


def get_all_frames(target, process):
    """Collect all valid coroutine frames from scheduler queues."""
    frames = []
    seen = set()
    
    for task_addr, worker_id in iterate_all_tasks(target, process):
        if task_addr in seen:
            continue
        seen.add(task_addr)
        
        info = get_frame_from_handle(process, task_addr)
        if info:
            info["queue_worker_id"] = worker_id
            frames.append((task_addr, info))
    
    return frames


# LLDB Command implementations

def elio_list(debugger, command, result, internal_dict):
    """List all Elio vthreads from worker queues."""
    target = debugger.GetSelectedTarget()
    process = target.GetProcess()

    if not process.IsValid():
        result.AppendMessage("Error: No process")
        return

    sched_addr = get_scheduler(target, process)
    if sched_addr is None:
        result.AppendMessage("Error: No active scheduler found")
        return

    result.AppendMessage("-" * 80)
    result.AppendMessage(f"{'ID':<8} {'State':<12} {'Worker':<8} {'Function':<30} {'Location'}")
    result.AppendMessage("-" * 80)

    count = 0
    has_debug_metadata_warning = False
    for task_addr, info in get_all_frames(target, process):
        count += 1
        func = info["function"] or "<unknown>"
        if len(func) > 28:
            func = func[:25] + "..."

        loc = ""
        if info.get("has_debug_metadata") and info["file"]:
            loc = f"{info['file']}"
            if info["line"] > 0:
                loc += f":{info['line']}"
        elif info.get("has_debug_metadata") is False:
            has_debug_metadata_warning = True

        worker = str(info.get("queue_worker_id", info["worker_id"]))

        result.AppendMessage(f"{info['id']:<8} {info['state']:<12} {worker:<8} {func:<30} {loc}")

    result.AppendMessage(f"\nTotal queued coroutines: {count}")
    if has_debug_metadata_warning:
        result.AppendMessage("(Note: Debug metadata is disabled - location info not available)")
    result.AppendMessage("Note: Only queued (not currently executing) coroutines are shown.")


def elio_bt(debugger, command, result, internal_dict):
    """Show backtrace for Elio vthread(s)."""
    target = debugger.GetSelectedTarget()
    process = target.GetProcess()

    if not process.IsValid():
        result.AppendMessage("Error: No process")
        return

    sched_addr = get_scheduler(target, process)
    if sched_addr is None:
        result.AppendMessage("Error: No active scheduler found")
        return

    target_id = None
    if command.strip():
        try:
            target_id = int(command.strip())
        except ValueError:
            result.AppendMessage(f"Error: Invalid vthread ID: {command}")
            return

    found = False
    for task_addr, info in get_all_frames(target, process):
        if target_id is not None and info["id"] != target_id:
            continue

        found = True
        worker_id = info.get("queue_worker_id", info["worker_id"])
        result.AppendMessage(f"vthread #{info['id']} [{info['state']}] (worker {worker_id})")

        stack = walk_virtual_stack(process, task_addr)
        for i, frame in enumerate(stack):
            func = frame["function"] or "<unknown>"
            loc = ""
            if frame.get("has_debug_metadata") and frame.get("file"):
                loc = f" at {frame['file']}"
                if frame["line"] > 0:
                    loc += f":{frame['line']}"

            result.AppendMessage(f"  #{i:<3} 0x{frame['address']:016x} in {func}{loc}")

        result.AppendMessage("")

    if not found:
        if target_id is not None:
            result.AppendMessage(f"Error: vthread #{target_id} not found in queues")
        else:
            result.AppendMessage("No queued vthreads found")


def elio_info(debugger, command, result, internal_dict):
    """Show detailed info for an Elio vthread."""
    if not command.strip():
        result.AppendMessage("Usage: elio info <vthread-id>")
        return
    
    try:
        target_id = int(command.strip())
    except ValueError:
        result.AppendMessage(f"Error: Invalid vthread ID: {command}")
        return
    
    target = debugger.GetSelectedTarget()
    process = target.GetProcess()
    
    if not process.IsValid():
        result.AppendMessage("Error: No process")
        return
    
    for task_addr, info in get_all_frames(target, process):
        if info["id"] != target_id:
            continue
        
        result.AppendMessage(f"vthread #{info['id']}")
        result.AppendMessage(f"  State:    {info['state']}")
        worker = info.get('queue_worker_id', info['worker_id'])
        result.AppendMessage(f"  Worker:   {worker}")
        result.AppendMessage(f"  Handle:   0x{info['address']:016x}")
        result.AppendMessage(f"  Promise:  0x{info['promise_addr']:016x}")
        
        if info["function"]:
            result.AppendMessage(f"  Function: {info['function']}")
        if info["file"]:
            loc = info["file"]
            if info["line"] > 0:
                loc += f":{info['line']}"
            result.AppendMessage(f"  Location: {loc}")
        
        result.AppendMessage(f"\n  Virtual Call Stack:")
        stack = walk_virtual_stack(process, task_addr)
        for i, frame in enumerate(stack):
            func = frame["function"] or "<unknown>"
            loc = ""
            if frame.get("file"):
                loc = f" at {frame['file']}"
                if frame["line"] > 0:
                    loc += f":{frame['line']}"
            result.AppendMessage(f"    #{i:<3} {func}{loc}")
        
        return
    
    result.AppendMessage(f"Error: vthread #{target_id} not found in queues")


def elio_workers(debugger, command, result, internal_dict):
    """Show worker thread information."""
    target = debugger.GetSelectedTarget()
    process = target.GetProcess()
    
    if not process.IsValid():
        result.AppendMessage("Error: No process")
        return
    
    sched_addr = get_scheduler(target, process)
    if sched_addr is None:
        result.AppendMessage("Error: No active scheduler found")
        return
    
    result.AppendMessage(f"Scheduler at: 0x{sched_addr:016x}")
    result.AppendMessage("-" * 60)
    
    # Count tasks per worker
    worker_tasks = {}
    for task_addr, info in get_all_frames(target, process):
        worker_id = info.get("queue_worker_id", 0)
        worker_tasks[worker_id] = worker_tasks.get(worker_id, 0) + 1
    
    result.AppendMessage(f"{'Worker':<8} {'Queued Tasks'}")
    result.AppendMessage("-" * 60)
    
    for worker_id in sorted(worker_tasks.keys()):
        result.AppendMessage(f"{worker_id:<8} {worker_tasks[worker_id]}")
    
    total = sum(worker_tasks.values())
    result.AppendMessage(f"\nTotal queued: {total}")


def elio_stats(debugger, command, result, internal_dict):
    """Show scheduler statistics."""
    target = debugger.GetSelectedTarget()
    process = target.GetProcess()
    
    if not process.IsValid():
        result.AppendMessage("Error: No process")
        return
    
    sched_addr = get_scheduler(target, process)
    if sched_addr is None:
        result.AppendMessage("Scheduler: not found")
        return
    
    result.AppendMessage(f"Scheduler: 0x{sched_addr:016x}")
    
    count = 0
    for _ in get_all_frames(target, process):
        count += 1
    
    result.AppendMessage(f"Total queued coroutines: {count}")


def elio_help(debugger, command, result, internal_dict):
    """Show help for Elio commands."""
    result.AppendMessage("Elio Coroutine Debugger")
    result.AppendMessage("")
    result.AppendMessage("Commands:")
    result.AppendMessage("  elio list              - List vthreads from worker queues")
    result.AppendMessage("  elio bt [id]           - Show backtrace for vthread(s)")
    result.AppendMessage("  elio info <id>         - Show detailed info for a vthread")
    result.AppendMessage("  elio workers           - Show worker thread information")
    result.AppendMessage("  elio stats             - Show scheduler statistics")
    result.AppendMessage("  elio help              - Show this help")
    result.AppendMessage("")
    result.AppendMessage("Note: Coroutine frames are found by traversing scheduler's worker queues.")
    result.AppendMessage("      Only queued (not currently executing) coroutines are shown.")


def __lldb_init_module(debugger, internal_dict):
    """Initialize the LLDB module."""
    debugger.HandleCommand('command script add -f elio_lldb.elio_list "elio list"')
    debugger.HandleCommand('command script add -f elio_lldb.elio_bt "elio bt"')
    debugger.HandleCommand('command script add -f elio_lldb.elio_info "elio info"')
    debugger.HandleCommand('command script add -f elio_lldb.elio_workers "elio workers"')
    debugger.HandleCommand('command script add -f elio_lldb.elio_stats "elio stats"')
    debugger.HandleCommand('command script add -f elio_lldb.elio_help "elio"')
    debugger.HandleCommand('command script add -f elio_lldb.elio_help "elio help"')
    
    print("Elio LLDB extension loaded. Type 'elio help' for commands.")
