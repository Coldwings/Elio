#!/usr/bin/env python3
"""
Elio Coroutine Library - GDB Python Extension

This script provides GDB commands for debugging Elio coroutines (vthreads).
It finds coroutine frames by traversing the scheduler's worker queues.

Usage:
    In GDB: source /path/to/elio-gdb.py

Commands:
    elio list              - List all vthreads from worker queues
    elio bt [id]           - Show backtrace for vthread(s)
    elio info <id>         - Show detailed info for a vthread
    elio workers           - Show worker thread information
    elio stats             - Show scheduler statistics

Works with both live processes and coredumps.

Note: Debug metadata (location, state, worker_id, id) is only available
when ELIO_ENABLE_DEBUG_METADATA=1 (default for Debug builds).
"""

import gdb

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


def frame_display_identity(info):
    """Return printable identity fields without inventing debug metadata."""
    frame_id = info.get("id")
    state = info.get("state")
    return ("?" if frame_id is None else str(frame_id), state or "unknown")

# Check if debug metadata is enabled in the build
def is_debug_metadata_enabled():
    """Check if ELIO_ENABLE_DEBUG_METADATA is enabled."""
    try:
        # Try to find the macro definition in compile commands or symbols
        # This is a best-effort check - we'll also detect at runtime
        return True  # Default assume enabled, detect via missing data below
    except:
        return False


def read_atomic(val):
    """Read value from std::atomic."""
    try:
        return int(val)
    except:
        pass
    try:
        return int(val["_M_b"]["_M_p"])
    except:
        pass
    try:
        return int(val["_M_i"])
    except:
        pass
    return None


def get_scheduler():
    """Find the current scheduler."""
    try:
        sched_ptr = gdb.parse_and_eval("elio::runtime::scheduler::current()")
        if int(sched_ptr) == 0:
            return None
        return sched_ptr.dereference()
    except:
        pass
    
    try:
        sched_ptr = gdb.parse_and_eval("elio::runtime::current_scheduler_")
        if int(sched_ptr) == 0:
            return None
        return sched_ptr.dereference()
    except:
        pass
    
    return None


def read_cstring(addr):
    """Read a null-terminated string from memory."""
    try:
        addr_val = int(addr)
    except:
        return None
    
    if addr_val == 0:
        return None
    
    try:
        return addr.string()
    except:
        pass
    
    try:
        inferior = gdb.selected_inferior()
        mem = inferior.read_memory(addr_val, 256)
        s = bytes(mem).split(b'\x00')[0].decode('utf-8', errors='replace')
        return s if s else None
    except:
        return None


def get_promise_layout(_ptr_size):
    """Resolve promise_base field offsets, with a stripped-binary fallback."""
    fallback = {
        "frame_magic_": 0,
        "parent_": 8,
        "has_debug_metadata": False,
        "has_execution_context": False,
    }

    try:
        promise_type = gdb.lookup_type("elio::coro::promise_base").strip_typedefs()
        offsets = {}
        for field in promise_type.fields():
            bitpos = getattr(field, "bitpos", None)
            if field.name and bitpos is not None:
                offsets[field.name] = int(bitpos) // 8

        if "frame_magic_" not in offsets or "parent_" not in offsets:
            return fallback

        has_debug_metadata = all(name in offsets for name in (
            "debug_location_", "debug_state_", "debug_worker_id_", "debug_id_"))
        offsets["has_debug_metadata"] = has_debug_metadata
        # Do not decode std::shared_ptr internals; their layout is standard
        # library specific. Presence still confirms symbols match the current
        # promise_base runtime-state boundary.
        offsets["has_execution_context"] = "execution_context_" in offsets
        return offsets
    except Exception:
        return fallback


def get_frame_from_handle(handle_addr):
    """Extract promise_base info from a coroutine handle address.

    Note: Debug metadata fields (location, state, worker_id, id) are only
    available when ELIO_ENABLE_DEBUG_METADATA=1. When disabled, these fields
    will return default/None values.
    """
    if handle_addr == 0:
        return None

    try:
        # Coroutine frame layout (typical):
        # - resume function pointer
        # - destroy function pointer
        # - promise object

        # Read pointer size
        ptr_size = gdb.lookup_type("void").pointer().sizeof

        # Promise is typically at offset 2*ptr_size (after resume and destroy)
        promise_addr = handle_addr + 2 * ptr_size
        layout = get_promise_layout(ptr_size)

        # Read magic to validate
        inferior = gdb.selected_inferior()
        magic_bytes = inferior.read_memory(
            promise_addr + layout["frame_magic_"], 8)
        magic = int.from_bytes(bytes(magic_bytes), 'little')

        if magic != FRAME_MAGIC:
            return None

        # Resolve field offsets from debug information when available. Without
        # type information, only the stable magic/parent prefix is safe to
        # inspect. Optional metadata may not be compiled in.

        # Read parent pointer (always present)
        parent_bytes = inferior.read_memory(
            promise_addr + layout["parent_"], ptr_size)
        parent = int.from_bytes(bytes(parent_bytes), 'little')

        has_debug_metadata = layout["has_debug_metadata"]
        file_ptr = 0
        func_ptr = 0
        line = 0
        state = None
        worker_id = None
        debug_id = None

        try:
            if not has_debug_metadata:
                raise ValueError("debug metadata is disabled")

            debug_location_offset = layout["debug_location_"]
            # Try reading debug_location.file pointer
            file_ptr_bytes = inferior.read_memory(promise_addr + debug_location_offset, ptr_size)
            file_ptr = int.from_bytes(bytes(file_ptr_bytes), 'little')

            func_ptr_bytes = inferior.read_memory(promise_addr + debug_location_offset + ptr_size, ptr_size)
            func_ptr = int.from_bytes(bytes(func_ptr_bytes), 'little')

            line_bytes = inferior.read_memory(promise_addr + debug_location_offset + 2 * ptr_size, 4)
            line = int.from_bytes(bytes(line_bytes), 'little')

            state_byte = inferior.read_memory(
                promise_addr + layout["debug_state_"], 1)
            state = int.from_bytes(bytes(state_byte), 'little')

            worker_id_bytes = inferior.read_memory(
                promise_addr + layout["debug_worker_id_"], 4)
            worker_id = int.from_bytes(bytes(worker_id_bytes), 'little')

            debug_id_bytes = inferior.read_memory(
                promise_addr + layout["debug_id_"], 8)
            debug_id = int.from_bytes(bytes(debug_id_bytes), 'little')
        except Exception:
            # Debug metadata is unavailable; retain explicit unknown values.
            has_debug_metadata = False

        # Read strings (only if debug metadata enabled)
        file_str = None
        func_str = None
        if has_debug_metadata and file_ptr != 0:
            try:
                file_mem = inferior.read_memory(file_ptr, 256)
                file_str = bytes(file_mem).split(b'\x00')[0].decode('utf-8', errors='replace')
            except:
                pass
        if has_debug_metadata and func_ptr != 0:
            try:
                func_mem = inferior.read_memory(func_ptr, 256)
                func_str = bytes(func_mem).split(b'\x00')[0].decode('utf-8', errors='replace')
            except:
                pass

        return {
            "id": debug_id,
            "state": (COROUTINE_STATES.get(state, "unknown")
                      if state is not None else None),
            "worker_id": worker_id,
            "parent": parent,
            "file": file_str,
            "function": func_str,
            "line": line,
            "address": handle_addr,
            "promise_addr": promise_addr,
            "has_debug_metadata": has_debug_metadata,
            "has_execution_context": layout["has_execution_context"]
        }
    except Exception as e:
        return None


def walk_virtual_stack(handle_addr):
    """Walk the virtual stack from a coroutine handle."""
    stack = []
    visited = set()

    info = get_frame_from_handle(handle_addr)
    if info:
        stack.append(info)
        visited.add(handle_addr)

        # Walk parent chain
        parent = info["parent"]
        while parent != 0 and parent not in visited:
            visited.add(parent)
            # Parent is a promise_base*, need to find the frame address
            # This is tricky - for now just note we have a parent
            stack.append({"id": 0, "address": parent, "state": "parent",
                         "function": None, "file": None, "line": 0, "worker_id": 0xFFFFFFFF,
                         "has_debug_metadata": False})
            break

    return stack


def iterate_chase_lev_deque(queue):
    """Iterate over entries in a Chase-Lev deque (gdb.Value of the deque).

    Layout (chase_lev_deque.hpp):
      * top_, bottom_, buffer_ are each ``alignas(64) std::atomic<...>``
        on their own cache line. We read them via gdb's atomic accessor.
      * buffer_ holds a ``circular_buffer*``. The pointee starts with
        ``capacity_`` (size_t), ``mask_`` (size_t), then a
        ``unique_ptr<atomic<T*>[]>`` whose held pointer is the slot array.

    Walking the slot array via raw memory reads keeps us ABI-agnostic across
    libstdc++ and libc++ unique_ptr layouts.
    """
    try:
        top = read_atomic(queue["top_"])
        bottom = read_atomic(queue["bottom_"])
        if top is None or bottom is None or top >= bottom:
            return

        buf_ptr_val = read_atomic(queue["buffer_"])
        if not buf_ptr_val:
            return

        ptr_size = gdb.lookup_type("void").pointer().sizeof
        inferior = gdb.selected_inferior()

        # circular_buffer: capacity_(size_t) at offset 0;
        # buffer_ (unique_ptr held ptr) at offset 2*sizeof(size_t).
        cap_bytes = inferior.read_memory(buf_ptr_val, 8)
        capacity = int.from_bytes(bytes(cap_bytes), 'little')
        if capacity == 0:
            return

        slots_bytes = inferior.read_memory(buf_ptr_val + 2 * 8, ptr_size)
        slots_addr = int.from_bytes(bytes(slots_bytes), 'little')
        if slots_addr == 0:
            return

        for i in range(top, bottom):
            try:
                idx = i % capacity
                raw = inferior.read_memory(slots_addr + idx * ptr_size, ptr_size)
                entry = int.from_bytes(bytes(raw), 'little')
                if entry:
                    yield entry
            except:
                pass
    except:
        pass


def iterate_worker_tasks(worker):
    """Iterate over all tasks in a worker's run queue.

    The MPSC inbox is intentionally not walked: its array-of-slots layout
    with embedded sequence counters is not safe to traverse lock-free from
    a debugger.
    """
    try:
        # queue_ is unique_ptr<chase_lev_deque<void>>; .get() yields the deque
        # pointer. Use parse_and_eval to remain ABI-agnostic.
        queue_ptr = gdb.parse_and_eval(f"(({worker.type}*)({int(worker.address)}))->queue_.get()")
        if int(queue_ptr) == 0:
            return
        for addr in iterate_chase_lev_deque(queue_ptr.dereference()):
            yield addr
    except:
        pass


def _array_get(arr_value, i):
    """Index into a std::array gdb.Value, handling libstdc++/libc++ ABIs.

    libstdc++ stores the data in ``_M_elems``; libc++ uses ``__elems_``. As of
    GDB 10+ pretty-printers expose ``__getitem__`` for std::array, but that's
    not universal — fall back to the ABI-specific member.
    """
    # Direct indexing first: works for raw arrays/pointers and for std::array
    # when a Python __getitem__ override is in scope.
    try:
        return arr_value[i]
    except Exception:
        pass
    for member in ("_M_elems", "__elems_"):
        try:
            return arr_value[member][i]
        except Exception:
            continue
    raise RuntimeError("std::array indexing failed (unknown stdlib ABI)")


def iterate_all_tasks(scheduler):
    """Iterate over all tasks across all workers.

    PR #72 changed ``workers_`` from ``std::vector<std::unique_ptr<worker_thread>>``
    to ``std::array<std::unique_ptr<worker_thread>, MAX_THREADS>``. The std::array
    holds storage inline; we index into it via the libstdc++/libc++ helper above
    and clamp the upper bound by ``min(num_threads_, MAX_THREADS=256)`` to stay
    within the array.
    """
    if scheduler is None:
        return

    try:
        num_threads = read_atomic(scheduler["num_threads_"])
        if num_threads is None or num_threads == 0:
            return
        # Clamp to the std::array's static capacity (scheduler::MAX_THREADS).
        if num_threads > 256:
            num_threads = 256

        workers = scheduler["workers_"]

        # unique_ptr<worker_thread>::get() is the stable accessor regardless
        # of whether libstdc++/libc++ exposes children() or __getitem__ for
        # the array. parse_and_eval lets us call it directly.
        sched_addr = int(scheduler.address)
        for i in range(num_threads):
            try:
                worker_ptr = gdb.parse_and_eval(
                    f"((elio::runtime::scheduler*)({sched_addr}))->workers_[{i}].get()"
                )
                if int(worker_ptr) == 0:
                    continue
                worker = worker_ptr.dereference()
                worker_id = int(worker["worker_id_"])

                for task_addr in iterate_worker_tasks(worker):
                    yield task_addr, worker_id
            except:
                # Fallback path when parse_and_eval can't materialize
                # workers_[i] (e.g. stripped binary): try _array_get directly.
                try:
                    slot = _array_get(workers, i)
                    # slot is unique_ptr<worker_thread>; pull the held ptr
                    # through .get() if available, otherwise skip.
                    worker_ptr = gdb.parse_and_eval(
                        f"((std::unique_ptr<elio::runtime::worker_thread>*)({int(slot.address)}))->get()"
                    )
                    if int(worker_ptr) == 0:
                        continue
                    worker = worker_ptr.dereference()
                    worker_id = int(worker["worker_id_"])
                    for task_addr in iterate_worker_tasks(worker):
                        yield task_addr, worker_id
                except:
                    pass
    except:
        pass


# ---------------------------------------------------------------------------
# Known limitation: I/O-suspended coroutines are NOT enumerated.
#
# A coroutine awaiting an io_uring SQE is not in any worker's run queue. Its
# handle is reachable only through ``op_state`` objects referenced by tagged
# ``user_data`` values inside the kernel-side io_uring CQ ring (see
# include/elio/io/io_uring_backend.hpp). The backend tracks a count
# (``pending_ops_``) but does not maintain a walkable container of in-flight
# op_states, so iterating them from a debugger would require walking
# io_uring's mmap'd CQ ring — kernel-version-dependent and out of scope.
# Stacks dominated by I/O suspension will therefore report few or no tasks
# via ``elio list``; combine with native ``thread apply all bt`` for
# coverage.
# ---------------------------------------------------------------------------


class ElioCommand(gdb.Command):
    """Elio coroutine debugging commands."""
    
    def __init__(self):
        super(ElioCommand, self).__init__(
            "elio",
            gdb.COMMAND_USER,
            gdb.COMPLETE_COMMAND,
            True
        )
    
    def invoke(self, arg, from_tty):
        print("Elio Coroutine Debugger")
        print("Usage:")
        print("  elio list              - List vthreads from worker queues")
        print("  elio bt [id]           - Show backtrace for vthread(s)")
        print("  elio info <id>         - Show detailed info for a vthread")
        print("  elio workers           - Show worker thread information")
        print("  elio stats             - Show scheduler statistics")
        print("")
        print("Note: Coroutine frames are found by traversing scheduler's worker queues.")
        print("      Only queued (not currently executing) coroutines are shown.")


class ElioListCommand(gdb.Command):
    """List all vthreads from worker queues."""

    def __init__(self):
        super(ElioListCommand, self).__init__(
            "elio list",
            gdb.COMMAND_USER
        )

    def invoke(self, arg, from_tty):
        scheduler = get_scheduler()
        if scheduler is None:
            print("Error: No active scheduler found")
            return

        print("-" * 80)
        print(f"{'ID':<8} {'State':<12} {'Worker':<8} {'Function':<30} {'Location'}")
        print("-" * 80)

        count = 0
        has_debug_metadata_warning = False
        for task_addr, worker_id in iterate_all_tasks(scheduler):
            info = get_frame_from_handle(task_addr)
            if info is None:
                continue

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

            worker = str(worker_id)

            display_id, display_state = frame_display_identity(info)
            print(f"{display_id:<8} {display_state:<12} {worker:<8} {func:<30} {loc}")

        print(f"\nTotal queued coroutines: {count}")
        if has_debug_metadata_warning:
            print("(Note: Debug metadata is unavailable - identity and location are unknown)")


class ElioBtCommand(gdb.Command):
    """Show backtrace for vthread(s)."""

    def __init__(self):
        super(ElioBtCommand, self).__init__(
            "elio bt",
            gdb.COMMAND_USER
        )

    def invoke(self, arg, from_tty):
        scheduler = get_scheduler()
        if scheduler is None:
            print("Error: No active scheduler found")
            return

        target_id = None
        if arg.strip():
            try:
                target_id = int(arg.strip())
            except ValueError:
                print(f"Error: Invalid vthread ID: {arg}")
                return

        found = False
        for task_addr, worker_id in iterate_all_tasks(scheduler):
            info = get_frame_from_handle(task_addr)
            if info is None:
                continue

            if target_id is not None and info["id"] != target_id:
                continue

            found = True
            display_id, display_state = frame_display_identity(info)
            print(f"vthread #{display_id} [{display_state}] (worker {worker_id})")

            stack = walk_virtual_stack(task_addr)
            for i, frame in enumerate(stack):
                func = frame["function"] or "<unknown>"
                loc = ""
                if frame.get("has_debug_metadata") and frame.get("file"):
                    loc = f" at {frame['file']}"
                    if frame["line"] > 0:
                        loc += f":{frame['line']}"

                print(f"  #{i:<3} 0x{frame['address']:016x} in {func}{loc}")
            print()

        if not found:
            if target_id is not None:
                print(f"Error: vthread #{target_id} not found in queues")
            else:
                print("No queued vthreads found")


class ElioWorkersCommand(gdb.Command):
    """Show worker thread information."""
    
    def __init__(self):
        super(ElioWorkersCommand, self).__init__(
            "elio workers",
            gdb.COMMAND_USER
        )
    
    def invoke(self, arg, from_tty):
        scheduler = get_scheduler()
        if scheduler is None:
            print("Error: No active scheduler found")
            return
        
        try:
            num_threads = read_atomic(scheduler["num_threads_"])
            running = read_atomic(scheduler["running_"])
            
            print(f"Scheduler: {'running' if running else 'stopped'}")
            print(f"Worker threads: {num_threads}")
            print("-" * 60)
            print(f"{'Worker':<8} {'Status':<12} {'Queue Size':<12} {'Tasks Executed'}")
            print("-" * 60)
            
            sched_addr = int(scheduler.address)
            # Clamp to scheduler::MAX_THREADS: workers_ is std::array<...,256>.
            walk_n = min(int(num_threads), 256)
            for i in range(walk_n):
                try:
                    worker_ptr = gdb.parse_and_eval(
                        f"((elio::runtime::scheduler*)({sched_addr}))->workers_[{i}].get()"
                    )
                    if int(worker_ptr) == 0:
                        continue
                    worker = worker_ptr.dereference()
                    is_running = read_atomic(worker["running_"])
                    tasks_exec = read_atomic(worker["tasks_executed_"])

                    # Count queue size
                    queue_count = 0
                    for _ in iterate_worker_tasks(worker):
                        queue_count += 1

                    status = "running" if is_running else "stopped"
                    print(f"{i:<8} {status:<12} {queue_count:<12} {tasks_exec}")
                except:
                    print(f"{i:<8} <error>")
        except Exception as e:
            print(f"Error: {e}")


class ElioInfoCommand(gdb.Command):
    """Show detailed info for a specific vthread."""

    def __init__(self):
        super(ElioInfoCommand, self).__init__(
            "elio info",
            gdb.COMMAND_USER
        )

    def invoke(self, arg, from_tty):
        if not arg.strip():
            print("Usage: elio info <vthread-id>")
            return

        try:
            target_id = int(arg.strip())
        except ValueError:
            print(f"Error: Invalid vthread ID: {arg}")
            return

        scheduler = get_scheduler()
        if scheduler is None:
            print("Error: No active scheduler found")
            return

        for task_addr, worker_id in iterate_all_tasks(scheduler):
            info = get_frame_from_handle(task_addr)
            if info is None:
                continue

            if info["id"] != target_id:
                continue

            display_id, display_state = frame_display_identity(info)
            print(f"vthread #{display_id}")
            print(f"  State:    {display_state}")
            print(f"  Worker:   {worker_id}")
            print(f"  Handle:   0x{info['address']:016x}")
            print(f"  Promise:  0x{info['promise_addr']:016x}")

            if info.get("has_debug_metadata"):
                if info["function"]:
                    print(f"  Function: {info['function']}")
                if info["file"]:
                    loc = info["file"]
                    if info["line"] > 0:
                        loc += f":{info['line']}"
                    print(f"  Location: {loc}")
            else:
                print("  (Debug metadata disabled - function/location not available)")

            print(f"\n  Virtual Call Stack:")
            stack = walk_virtual_stack(task_addr)
            for i, frame in enumerate(stack):
                func = frame["function"] or "<unknown>"
                loc = ""
                if frame.get("has_debug_metadata") and frame.get("file"):
                    loc = f" at {frame['file']}"
                    if frame["line"] > 0:
                        loc += f":{frame['line']}"
                print(f"    #{i:<3} {func}{loc}")

            return

        print(f"Error: vthread #{target_id} not found in queues")


class ElioStatsCommand(gdb.Command):
    """Show scheduler statistics."""
    
    def __init__(self):
        super(ElioStatsCommand, self).__init__(
            "elio stats",
            gdb.COMMAND_USER
        )
    
    def invoke(self, arg, from_tty):
        scheduler = get_scheduler()
        if scheduler is None:
            print("Scheduler: not found")
            return
        
        try:
            num_threads = read_atomic(scheduler["num_threads_"])
            running = read_atomic(scheduler["running_"])
            
            print(f"Scheduler: {'running' if running else 'stopped'}")
            print(f"Worker threads: {num_threads}")
            
            total_queued = 0
            total_executed = 0
            sched_addr = int(scheduler.address)
            # Clamp to scheduler::MAX_THREADS: workers_ is std::array<...,256>.
            walk_n = min(int(num_threads), 256)
            for i in range(walk_n):
                try:
                    worker_ptr = gdb.parse_and_eval(
                        f"((elio::runtime::scheduler*)({sched_addr}))->workers_[{i}].get()"
                    )
                    if int(worker_ptr) == 0:
                        continue
                    worker = worker_ptr.dereference()
                    tasks_exec = read_atomic(worker["tasks_executed_"])
                    if tasks_exec:
                        total_executed += tasks_exec

                    for _ in iterate_worker_tasks(worker):
                        total_queued += 1
                except:
                    pass
            
            print(f"Total queued coroutines: {total_queued}")
            print(f"Total tasks executed: {total_executed}")
        except Exception as e:
            print(f"Error: {e}")


# Register commands
ElioCommand()
ElioListCommand()
ElioBtCommand()
ElioInfoCommand()
ElioWorkersCommand()
ElioStatsCommand()

print("Elio GDB extension loaded. Type 'elio' for help.")
