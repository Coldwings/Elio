#!/usr/bin/env python3
"""
Elio Coroutine Library - LLDB Python Extension

This script provides LLDB commands for debugging Elio coroutines (vthreads).
It finds coroutine frames by traversing the scheduler's worker queues.

Usage:
    In LLDB: command script import /path/to/elio-lldb.py

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
    """Extract promise_base info from a coroutine handle address."""
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
        
        # Read promise_base fields
        # Layout: magic(8) + parent(8) + exception(16) + debug_location(24) + state(1) + pad(3) + worker_id(4) + debug_id(8)
        
        parent = read_pointer(process, promise_addr + 8)
        
        # debug_location starts at offset 8+8+16=32
        loc_offset = 8 + ptr_size + 16  # magic + parent + exception_ptr
        file_ptr = read_pointer(process, promise_addr + loc_offset)
        func_ptr = read_pointer(process, promise_addr + loc_offset + ptr_size)
        line = read_uint32(process, promise_addr + loc_offset + 2 * ptr_size)
        
        # state at loc_offset + 24
        state_offset = loc_offset + 2 * ptr_size + 4
        state = read_uint8(process, promise_addr + state_offset)
        
        # worker_id at state_offset + 4 (after 3 bytes padding)
        worker_id = read_uint32(process, promise_addr + state_offset + 4)
        
        # debug_id at worker_id + 4
        debug_id = read_uint64(process, promise_addr + state_offset + 8)
        
        return {
            "id": debug_id,
            "state": COROUTINE_STATES.get(state, "unknown"),
            "worker_id": worker_id,
            "parent": parent,
            "file": read_cstring(process, file_ptr),
            "function": read_cstring(process, func_ptr),
            "line": line,
            "address": handle_addr,
            "promise_addr": promise_addr
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
                         "function": None, "file": None, "line": 0, "worker_id": 0xFFFFFFFF})
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
    """Iterate over entries in a Chase-Lev deque."""
    try:
        # Chase-Lev deque layout (approximate):
        # - top_ (atomic<size_t>)
        # - bottom_ (atomic<size_t>)
        # - buffer_ (struct with buffer_ pointer and capacity_)
        
        top = read_uint64(process, queue_addr)
        bottom = read_uint64(process, queue_addr + 8)
        
        # buffer_ offset depends on atomics size, typically 16
        buffer_struct_addr = queue_addr + 16
        buffer_ptr = read_pointer(process, buffer_struct_addr)
        capacity = read_uint64(process, buffer_struct_addr + ptr_size)
        
        if top is None or bottom is None or buffer_ptr == 0 or capacity == 0:
            return
        
        for i in range(top, bottom):
            try:
                idx = i % capacity
                entry_addr = buffer_ptr + idx * ptr_size
                entry = read_pointer(process, entry_addr)
                if entry and entry != 0:
                    yield entry
            except:
                pass
    except:
        pass


def iterate_mpsc_queue(process, inbox_addr, ptr_size):
    """Iterate over entries in MPSC queue (best effort)."""
    try:
        # MPSC queue structure varies, try common patterns
        head = read_uint64(process, inbox_addr)
        tail = read_uint64(process, inbox_addr + 8)
        buffer_ptr = read_pointer(process, inbox_addr + 16)
        capacity = read_uint64(process, inbox_addr + 16 + ptr_size)
        
        if head is None or tail is None or buffer_ptr == 0 or capacity == 0:
            return
        
        count = (tail - head) % capacity
        for i in range(count):
            try:
                idx = (head + i) % capacity
                entry_addr = buffer_ptr + idx * ptr_size
                entry = read_pointer(process, entry_addr)
                if entry and entry != 0:
                    yield entry
            except:
                pass
    except:
        pass


def iterate_worker_tasks(process, worker_addr, ptr_size):
    """Iterate over all tasks in a worker's queues."""
    try:
        # Worker layout depends on implementation
        # Try to find queue_ and inbox_ members
        # This is a best-effort approach based on typical layouts
        
        # Assume queue_ is at some offset after worker_id_ and other fields
        # We'll try multiple offsets
        
        for queue_offset in [8, 16, 24, 32, 40, 48]:
            queue_addr = worker_addr + queue_offset
            found_tasks = False
            for addr in iterate_chase_lev_deque(process, queue_addr, ptr_size):
                found_tasks = True
                yield addr
            if found_tasks:
                break
        
        # Try inbox_ at various offsets after queue_
        for inbox_offset in [64, 72, 80, 88, 96, 128, 256]:
            inbox_addr = worker_addr + inbox_offset
            for addr in iterate_mpsc_queue(process, inbox_addr, ptr_size):
                yield addr
    except:
        pass


def iterate_all_tasks(target, process):
    """Iterate over all tasks across all workers."""
    sched_addr = get_scheduler(target, process)
    if sched_addr is None:
        return
    
    ptr_size = process.GetAddressByteSize()
    
    try:
        # Scheduler layout (approximate):
        # Various fields, then:
        # - num_threads_ (atomic<size_t>)
        # - workers_ (unique_ptr<worker_thread>[] or vector)
        
        # Try to find num_threads_ and workers_ at various offsets
        # This is implementation-dependent
        
        for num_threads_offset in [0, 8, 16, 24, 32, 40, 48]:
            num_threads = read_uint64(process, sched_addr + num_threads_offset)
            if num_threads and 0 < num_threads <= 256:
                # Likely found num_threads, workers_ should be nearby
                workers_offset = num_threads_offset + 8
                workers_ptr = read_pointer(process, sched_addr + workers_offset)
                
                if workers_ptr != 0:
                    for i in range(num_threads):
                        try:
                            worker_ptr_addr = workers_ptr + i * ptr_size
                            worker_ptr = read_pointer(process, worker_ptr_addr)
                            
                            if worker_ptr == 0:
                                continue
                            
                            # Read worker_id_ (usually first field)
                            worker_id = read_uint32(process, worker_ptr)
                            
                            for task_addr in iterate_worker_tasks(process, worker_ptr, ptr_size):
                                yield task_addr, worker_id
                        except:
                            pass
                    return
    except:
        pass


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
    for task_addr, info in get_all_frames(target, process):
        count += 1
        func = info["function"] or "<unknown>"
        if len(func) > 28:
            func = func[:25] + "..."
        
        loc = ""
        if info["file"]:
            loc = f"{info['file']}"
            if info["line"] > 0:
                loc += f":{info['line']}"
        
        worker = str(info.get("queue_worker_id", info["worker_id"]))
        
        result.AppendMessage(f"{info['id']:<8} {info['state']:<12} {worker:<8} {func:<30} {loc}")
    
    result.AppendMessage(f"\nTotal queued coroutines: {count}")
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
            if frame.get("file"):
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
