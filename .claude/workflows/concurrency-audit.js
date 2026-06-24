
export const meta = {
  name: 'concurrency-audit',
  description: 'Deep audit of Elio coroutine library for concurrency and correctness bugs',
  phases: [
    { title: 'Scan', detail: 'Parallel deep-read of all concurrency-critical headers' },
    { title: 'Verify', detail: 'Adversarial verification of each finding' },
    { title: 'Synthesize', detail: 'Deduplicate, rank, and produce final report' }
  ]
};

// Phase 1: Fan-out scanners across all critical subsystems
phase('Scan');

const SCAN_PROMPTS = [
  {
    key: 'chase_lev_deque',
    files: ['include/elio/runtime/chase_lev_deque.hpp', 'include/elio/runtime/worker_thread.hpp'],
    prompt: `Deep-audit the Chase-Lev work-stealing deque and worker_thread for concurrency bugs.
Focus on:
1. Memory ordering: are all atomic operations using the correct memory_order? Any missing fences?
2. ABA problem: could steal() race with push()/take() causing use-after-free or stale reads?
3. Resize/grow races: when the buffer grows, can a concurrent steal see a partially-updated state?
4. Off-by-one in empty/full checks
5. Data races on non-atomic fields accessed from both owner and thief threads
6. Is the circular buffer indexing correct with wrap-around?

Read ALL files listed. For each real bug found, report: file, line range, severity (critical/high/medium), description, and a concrete fix sketch. Do NOT report style issues. Only report REAL bugs that could cause crashes, data corruption, or incorrect behavior under concurrent use.`
  },
  {
    key: 'scheduler_spawn',
    files: ['include/elio/runtime/scheduler.hpp', 'include/elio/runtime/spawn.hpp', 'include/elio/runtime/spawn_blocking.hpp', 'include/elio/runtime/blocking_pool.hpp'],
    prompt: `Deep-audit the scheduler, spawn, spawn_blocking, and blocking_pool for concurrency bugs.
Focus on:
1. Task submission races: can a task be submitted to a worker that is shutting down or not yet started?
2. Double-free or use-after-free of task objects across spawn/join paths
3. Shutdown races: what happens to in-flight tasks when the scheduler stops?
4. blocking_pool: can threads leak? Can a blocking task deadlock waiting for a worker?
5. Work distribution: are there races in how tasks are pushed to worker queues?
6. Lifetime issues: does the scheduler outlive all its workers and tasks?

Read ALL files listed. For each real bug found, report: file, line range, severity (critical/high/medium), description, and a concrete fix sketch. Only report REAL concurrency/correctness bugs.`
  },
  {
    key: 'sync_primitives',
    files: ['include/elio/sync/mutex.hpp', 'include/elio/sync/shared_mutex.hpp', 'include/elio/sync/semaphore.hpp', 'include/elio/sync/event.hpp', 'include/elio/sync/condition_variable.hpp', 'include/elio/sync/spinlock.hpp'],
    prompt: `Deep-audit all synchronization primitives for concurrency bugs.
Focus on:
1. Lost wakeups: can a waiter miss a signal and sleep forever?
2. Thundering herd: are wakeups targeted or broadcast where they should be?
3. Priority inversion or fairness issues
4. Deadlock potential: lock ordering, recursive acquisition
5. Cancellation safety: what happens if a coroutine is cancelled while holding/waiting for a lock?
6. shared_mutex: reader/writer races, upgrade/downgrade correctness
7. condition_variable: spurious wakeup handling, predicate re-check
8. semaphore: count correctness under concurrent signal/wait

Read ALL files listed. For each real bug found, report: file, line range, severity, description, and fix sketch. Only REAL bugs.`
  },
  {
    key: 'channel',
    files: ['include/elio/sync/channel.hpp'],
    prompt: `Deep-audit the bounded channel (channel<T>) for concurrency bugs.
Focus on:
1. Producer/consumer races: can items be lost or duplicated?
2. Bounded buffer: off-by-one in capacity checks, full/empty confusion
3. Wakeup logic: can a blocked sender or receiver miss a wakeup?
4. Cancellation: what happens to pending send/receive when channel closes?
5. Move semantics: are items moved correctly without double-free?
6. Close semantics: does close unblock all waiters correctly?

Read the file thoroughly. For each real bug found, report: file, line range, severity, description, and fix sketch. Only REAL concurrency bugs.`
  },
  {
    key: 'coro_lifetime',
    files: ['include/elio/coro/task.hpp', 'include/elio/coro/promise_base.hpp', 'include/elio/coro/task_handle.hpp', 'include/elio/coro/when_all.hpp', 'include/elio/coro/when_any.hpp', 'include/elio/coro/with_timeout.hpp'],
    prompt: `Deep-audit coroutine lifetime management for correctness bugs.
Focus on:
1. Dangling references: can a task reference a destroyed coroutine frame?
2. Exception propagation: are exceptions correctly forwarded across co_await boundaries?
3. when_all/when_any: what happens if one sub-task throws? Are remaining tasks cancelled/cleaned up?
4. with_timeout: if the timeout fires, is the inner task properly destroyed? Can it complete after timeout?
5. Promise base: virtual stack linking — can the chain become corrupted?
6. task_handle: can it outlive the task it references?
7. Symmetric transfer: are resume/suspend correctly paired?

Read ALL files. For each real bug, report: file, line range, severity, description, fix sketch. Only REAL bugs.`
  },
  {
    key: 'io_backend',
    files: ['include/elio/io/io_context.hpp', 'include/elio/io/io_uring_backend.hpp', 'include/elio/io/epoll_backend.hpp', 'include/elio/io/io_awaitables.hpp'],
    prompt: `Deep-audit the I/O backends and awaitables for concurrency bugs.
Focus on:
1. io_uring CQ processing: can completions be lost or processed twice?
2. Epoll edge cases: level vs edge triggered, missed events
3. Cancellation races: what happens if an I/O is cancelled after submission but before completion?
4. Timer correctness: can a timer fire early or be lost?
5. Backend selection: is the fallback from io_uring to epoll race-free?
6. File descriptor lifetime: can an fd be closed while I/O is in-flight?
7. SQE submission: are submissions atomic w.r.t. the ring buffer?

Read ALL files. Report only REAL bugs with file, line, severity, description, fix sketch.`
  },
  {
    key: 'autoscaler_mpsc',
    files: ['include/elio/runtime/autoscaler.hpp', 'include/elio/runtime/autoscaler_config.hpp', 'include/elio/runtime/autoscaler_triggers.hpp', 'include/elio/runtime/autoscaler_actions.hpp', 'include/elio/runtime/mpsc_queue.hpp', 'include/elio/runtime/wait_strategy.hpp'],
    prompt: `Deep-audit the autoscaler, MPSC queue, and wait strategy for concurrency bugs.
Focus on:
1. MPSC queue: lost items, ABA problem, memory ordering
2. Autoscaler: can it add/remove workers while tasks are in-flight? Race between scale-up and scale-down decisions?
3. Wait strategy: can a worker sleep through a wakeup? Busy-spin when it should sleep?
4. Worker removal: when a worker is removed, what happens to its pending tasks?
5. Configuration races: can config changes during scaling cause inconsistent state?

Read ALL files. Report only REAL bugs with file, line, severity, description, fix sketch.`
  },
  {
    key: 'cancel_token_generator',
    files: ['include/elio/coro/cancel_token.hpp', 'include/elio/coro/generator.hpp'],
    prompt: `Deep-audit cancel_token and generator for concurrency bugs.
Focus on:
1. cancel_token: race between request_cancel() and cancel_requested() checks
2. cancel_token: can cancellation be lost if requested before any waiter registers?
3. generator: symmetric transfer correctness — can the generator and caller both be suspended?
4. generator: what happens if the generator throws during iteration?
5. generator: lifetime — can the generator be destroyed while a consumer is awaiting?

Read ALL files. Report only REAL bugs with file, line, severity, description, fix sketch.`
  }
];

const scanResults = await parallel(
  SCAN_PROMPTS.map(s => () =>
    agent(s.prompt, {
      label: `scan:${s.key}`,
      phase: 'Scan',
      schema: {
        type: 'object',
        properties: {
          findings: {
            type: 'array',
            items: {
              type: 'object',
              properties: {
                key: { type: 'string' },
                file: { type: 'string' },
                lines: { type: 'string' },
                severity: { type: 'string', enum: ['critical', 'high', 'medium'] },
                description: { type: 'string' },
                fix_sketch: { type: 'string' },
                code_snippet: { type: 'string' }
              },
              required: ['key', 'file', 'lines', 'severity', 'description', 'fix_sketch']
            }
          }
        },
        required: ['findings']
      }
    })
  )
);

// Collect all findings
const allFindings = scanResults
  .filter(Boolean)
  .flatMap(r => r.findings || []);

log(`Phase 1 complete: ${allFindings.length} raw findings across all scanners`);

// Phase 2: Adversarial verification — each finding gets 2 independent skeptics
phase('Verify');

const verified = await pipeline(
  allFindings,
  finding => parallel([
    () => agent(
      `You are a concurrency expert skeptic. Try to REFUTE this finding. Read the file and determine if the reported bug is REAL or a FALSE POSITIVE.

Finding: ${finding.file}:${finding.lines}
Severity claimed: ${finding.severity}
Description: ${finding.description}
Code: ${finding.code_snippet || 'see file'}

Read the file at the reported lines. Consider: Is there a subtle correctness argument the finder missed? Does the C++ memory model actually guarantee safety here? Is this a known-safe pattern?

Respond with: refuted (true/false), confidence (0-1), reason (one sentence).`,
      {
        label: `verify1:${finding.key}`,
        phase: 'Verify',
        schema: {
          type: 'object',
          properties: {
            refuted: { type: 'boolean' },
            confidence: { type: 'number' },
            reason: { type: 'string' }
          },
          required: ['refuted', 'confidence', 'reason']
        }
      }
    ),
    () => agent(
      `You are a systems programmer skeptic. Try to REFUTE this finding. Read the file and determine if the reported bug is REAL or a FALSE POSITIVE.

Finding: ${finding.file}:${finding.lines}
Severity claimed: ${finding.severity}
Description: ${finding.description}
Code: ${finding.code_snippet || 'see file'}

Read the file at the reported lines. Consider: Could this be a theoretical-only issue that never manifests in practice? Is there existing mitigation (e.g., the caller already prevents the race)? Is the fix sketch correct?

Respond with: refuted (true/false), confidence (0-1), reason (one sentence).`,
      {
        label: `verify2:${finding.key}`,
        phase: 'Verify',
        schema: {
          type: 'object',
          properties: {
            refuted: { type: 'boolean' },
            confidence: { type: 'number' },
            reason: { type: 'string' }
          },
          required: ['refuted', 'confidence', 'reason']
        }
      }
    )
  ]).then(([v1, v2]) => {
    const votes = [v1, v2].filter(Boolean);
    const refuted = votes.filter(v => v.refuted).length;
    const survived = refuted === 0; // Both skeptics must refute to kill
    return { ...finding, survived, votes };
  })
);

const confirmed = verified.filter(Boolean).filter(f => f.survived);

log(`Phase 2 complete: ${confirmed.length} confirmed findings out of ${allFindings.length}`);

// Phase 3: Synthesize — deduplicate, rank, produce actionable report
phase('Synthesize');

// Group by file
const byFile = {};
for (const f of confirmed) {
  const k = f.file;
  if (!byFile[k]) byFile[k] = [];
  byFile[k].push(f);
}

const ranked = Object.entries(byFile).flatMap(([file, findings]) =>
  findings.sort((a, b) => {
    const sev = { critical: 0, high: 1, medium: 2 };
    return (sev[a.severity] ?? 3) - (sev[b.severity] ?? 3);
  })
);

log(`Final report: ${ranked.length} confirmed bugs across ${Object.keys(byFile).length} files`);

return {
  total_raw: allFindings.length,
  confirmed: ranked.length,
  by_file: byFile,
  findings: ranked
};
