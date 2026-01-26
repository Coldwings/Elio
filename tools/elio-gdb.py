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


def get_frame_from_handle(handle_addr):
    """Extract promise_base info from a coroutine handle address."""
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
        
        # Read magic to validate
        inferior = gdb.selected_inferior()
        magic_bytes = inferior.read_memory(promise_addr, 8)
        magic = int.from_bytes(bytes(magic_bytes), 'little')
        
        if magic != FRAME_MAGIC:
            return None
        
        # Read promise_base fields
        # Layout: magic(8) + parent(8) + exception(16) + debug_location(24) + state(1) + pad(3) + worker_id(4) + debug_id(8)
        
        parent_bytes = inferior.read_memory(promise_addr + 8, ptr_size)
        parent = int.from_bytes(bytes(parent_bytes), 'little')
        
        # debug_location starts at offset 8+8+16=32
        loc_offset = 8 + ptr_size + 16  # magic + parent + exception_ptr
        file_ptr_bytes = inferior.read_memory(promise_addr + loc_offset, ptr_size)
        file_ptr = int.from_bytes(bytes(file_ptr_bytes), 'little')
        
        func_ptr_bytes = inferior.read_memory(promise_addr + loc_offset + ptr_size, ptr_size)
        func_ptr = int.from_bytes(bytes(func_ptr_bytes), 'little')
        
        line_bytes = inferior.read_memory(promise_addr + loc_offset + 2 * ptr_size, 4)
        line = int.from_bytes(bytes(line_bytes), 'little')
        
        # state at loc_offset + 24
        state_offset = loc_offset + 2 * ptr_size + 4
        state_byte = inferior.read_memory(promise_addr + state_offset, 1)
        state = int.from_bytes(bytes(state_byte), 'little')
        
        # worker_id at state_offset + 4 (after 3 bytes padding)
        worker_id_bytes = inferior.read_memory(promise_addr + state_offset + 4, 4)
        worker_id = int.from_bytes(bytes(worker_id_bytes), 'little')
        
        # debug_id at worker_id + 4
        debug_id_bytes = inferior.read_memory(promise_addr + state_offset + 8, 8)
        debug_id = int.from_bytes(bytes(debug_id_bytes), 'little')
        
        # Read strings
        file_str = None
        func_str = None
        if file_ptr != 0:
            try:
                file_mem = inferior.read_memory(file_ptr, 256)
                file_str = bytes(file_mem).split(b'\x00')[0].decode('utf-8', errors='replace')
            except:
                pass
        if func_ptr != 0:
            try:
                func_mem = inferior.read_memory(func_ptr, 256)
                func_str = bytes(func_mem).split(b'\x00')[0].decode('utf-8', errors='replace')
            except:
                pass
        
        return {
            "id": debug_id,
            "state": COROUTINE_STATES.get(state, "unknown"),
            "worker_id": worker_id,
            "parent": parent,
            "file": file_str,
            "function": func_str,
            "line": line,
            "address": handle_addr,
            "promise_addr": promise_addr
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
                         "function": None, "file": None, "line": 0, "worker_id": 0xFFFFFFFF})
            break
    
    return stack


def iterate_chase_lev_deque(queue):
    """Iterate over entries in a Chase-Lev deque."""
    try:
        top = read_atomic(queue["top_"])
        bottom = read_atomic(queue["bottom_"])
        buffer = queue["buffer_"]["buffer_"]
        capacity = int(queue["buffer_"]["capacity_"])
        
        if top is None or bottom is None:
            return
        
        for i in range(top, bottom):
            try:
                idx = i % capacity
                entry = read_atomic(buffer[idx])
                if entry and entry != 0:
                    yield entry
            except:
                pass
    except:
        pass


def iterate_mpsc_queue(inbox):
    """Iterate over entries in MPSC queue (best effort)."""
    try:
        # MPSC queue structure varies, try common patterns
        head = read_atomic(inbox["head_"])
        tail = read_atomic(inbox["tail_"])
        buffer = inbox["buffer_"]
        capacity = int(inbox["capacity_"])
        
        if head is None or tail is None:
            return
        
        count = (tail - head) % capacity
        for i in range(count):
            try:
                idx = (head + i) % capacity
                entry_addr = read_atomic(buffer[idx])
                if entry_addr and entry_addr != 0:
                    yield entry_addr
            except:
                pass
    except:
        pass


def iterate_worker_tasks(worker):
    """Iterate over all tasks in a worker's queues."""
    try:
        # Iterate Chase-Lev deque
        queue = worker["queue_"]
        for addr in iterate_chase_lev_deque(queue):
            yield addr
        
        # Iterate MPSC inbox
        inbox = worker["inbox_"]
        for addr in iterate_mpsc_queue(inbox):
            yield addr
    except:
        pass


def iterate_all_tasks(scheduler):
    """Iterate over all tasks across all workers."""
    if scheduler is None:
        return
    
    try:
        num_threads = read_atomic(scheduler["num_threads_"])
        workers = scheduler["workers_"]
        
        if num_threads is None:
            return
        
        for i in range(num_threads):
            try:
                worker_ptr = workers[i]
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
        for task_addr, worker_id in iterate_all_tasks(scheduler):
            info = get_frame_from_handle(task_addr)
            if info is None:
                continue
            
            count += 1
            func = info["function"] or "<unknown>"
            if len(func) > 28:
                func = func[:25] + "..."
            
            loc = ""
            if info["file"]:
                loc = f"{info['file']}"
                if info["line"] > 0:
                    loc += f":{info['line']}"
            
            worker = str(worker_id)
            
            print(f"{info['id']:<8} {info['state']:<12} {worker:<8} {func:<30} {loc}")
        
        print(f"\nTotal queued coroutines: {count}")


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
            print(f"vthread #{info['id']} [{info['state']}] (worker {worker_id})")
            
            stack = walk_virtual_stack(task_addr)
            for i, frame in enumerate(stack):
                func = frame["function"] or "<unknown>"
                loc = ""
                if frame.get("file"):
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
            
            workers = scheduler["workers_"]
            for i in range(num_threads):
                try:
                    worker_ptr = workers[i]
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
            
            print(f"vthread #{info['id']}")
            print(f"  State:    {info['state']}")
            print(f"  Worker:   {worker_id}")
            print(f"  Handle:   0x{info['address']:016x}")
            print(f"  Promise:  0x{info['promise_addr']:016x}")
            
            if info["function"]:
                print(f"  Function: {info['function']}")
            if info["file"]:
                loc = info["file"]
                if info["line"] > 0:
                    loc += f":{info['line']}"
                print(f"  Location: {loc}")
            
            print(f"\n  Virtual Call Stack:")
            stack = walk_virtual_stack(task_addr)
            for i, frame in enumerate(stack):
                func = frame["function"] or "<unknown>"
                loc = ""
                if frame.get("file"):
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
            workers = scheduler["workers_"]
            for i in range(num_threads):
                try:
                    worker_ptr = workers[i]
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
