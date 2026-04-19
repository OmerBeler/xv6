# Threads in xv6

User-level threads sharing an address space, implemented as separate
`struct proc` slots in the process table.

## Quick start

From the xv6 shell:

```
$ ttest
```

From C:

```c
#include "types.h"
#include "user.h"

static char stack[4096] __attribute__((aligned(16)));

static void *
worker(void)
{
    // ...work...
    thread_exit((void*)42);
}

int
main(void)
{
    tid_t tid;
    void *rv;
    thread_create(&tid, worker, stack, sizeof(stack));
    thread_join(tid, &rv);    // rv == (void*)42
    exit();
}
```

## API

Declared in `user.h`:

| Call | Contract |
|------|----------|
| `int thread_create(tid_t *tid, void *(*entry)(void), void *stack, uint stack_size)` | Create a sibling thread in the current process. On success writes the new tid to `*tid` and returns 0. Returns -1 if the process table is full or arguments are invalid. |
| `void thread_exit(void *exit_value)` | Terminate the calling thread. `exit_value` is stored for a future `thread_join`. Does not return. If the caller is the only thread left in the process, the process ends and the parent's `wait()` unblocks. |
| `int thread_join(tid_t tid, void **exit_value)` | Block until the target thread (must be in the same process) is ZOMBIE. On success returns 0 and, if `exit_value` is non-NULL, writes the thread's exit value there. Returns -1 if `tid` is not in the group, the caller tries to join itself, or the caller is killed while waiting. |
| `uint thread_getThreadId(void)` | Current thread's tid. |
| `uint thread_getProcessId(void)` | The enclosing process's pid (the leader thread's pid). |

`tid_t` is `typedef uint tid_t;` — tids live in the same namespace as
pids, since every thread is a `struct proc` with its own `p->pid`.

The entry function is called with no arguments. If it returns via a
plain `ret` the thread faults (the stack's bottom holds `0xFFFFFFFF`
as a sentinel return PC); always call `thread_exit` explicitly.

## Semantics

### Sharing

Threads in the same group share:

- the page directory (`pgdir`) and address-space size (`sz`)
- the `parent` pointer (the enclosing process's parent)

Threads do **not** share:

- the open-file table — each thread gets a private `ofile[]` seeded at
  `thread_create` time via `filedup` on every fd, and its own `cwd`
  via `idup`. This differs from POSIX (where threads share fds); it is
  a deliberate teaching-OS simplification so the existing xv6 file
  ref-counting continues to work without a new layer of indirection.
- the kernel stack, trap frame, or scheduling context.

### Leader vs thread slots

One `struct proc` in the group is the **leader** (`is_thread == 0`).
The leader is the slot that was originally allocated by `fork()` or
`userinit()`. Its `pid` is the process's pid.

Each additional thread is a `struct proc` with `is_thread == 1` and
`thread_group` pointing at the leader. Its `pid` is its tid.

`thread_leader(p)` returns the group leader for any slot:

```c
struct proc *thread_leader(struct proc *p) {
    return p->is_thread ? p->thread_group : p;
}
```

### Thread lifecycle

| Event | Effect |
|-------|--------|
| `thread_create` | allocate a slot, share pgdir/sz, filedup the fds, idup the cwd, `leader->thread_count++`, RUNNABLE |
| `thread_exit` | close own fds, iput own cwd, record exit value, `leader->thread_count--`, wake joiner + group chan + parent-if-last, ZOMBIE |
| `thread_join` (non-leader target) | sleep on target; on ZOMBIE read the exit value, `kfree(kstack)`, reset fields, UNUSED |
| `thread_join` (leader target) | same, but leader slot is **not** reaped here — the parent's `wait()` owns the process-level reap |
| `exit()` from any thread | kills every group member (killed=1, SLEEPING->RUNNABLE); each member runs its own `exit()` on trap return and closes its own fds/cwd before going ZOMBIE |
| parent's `wait()` | skips `is_thread` slots; for a ZOMBIE leader, only reaps when every group member is ZOMBIE, then reaps the whole group and `freevm`s the shared pgdir once |
| `kill(pid)` | POSIX-like: kills the whole group, not just one thread |
| image swap from any thread | drains the group (kills and reaps all siblings) just before the pgdir swap, so no thread is left running on the memory we're about to free |

### `sbrk`/`growproc`

`sbrk` from any thread grows the shared pgdir. The new `sz` is
propagated to every slot in the group under `ptable.lock` so that
`argptr`/`argint` bounds checks stay consistent. Concurrent `sbrk`
calls from multiple threads are serialized by `ptable.lock`.

### `getpid`

`getpid()` returns the leader's pid (the process id). For the
thread's own id use `thread_getThreadId()`.

## Implementation map

| File | What it contributes |
|------|---------------------|
| `proc.h` | `tid_t` typedef; `is_thread`, `thread_group`, `thread_count`, `thread_exit_value`, `thread_exited` fields on `struct proc` |
| `proc.c` | `thread_leader`, `thread_create`, `thread_exit`, `thread_join`, `thread_getThreadId`, `thread_getProcessId`, `drain_thread_group`, thread-aware `fork`/`exit`/`wait`/`kill`/`growproc`, thread-field init in `allocproc` |
| `exec.c` | calls `drain_thread_group()` right before the pgdir swap |
| `sysproc.c` | `sys_thread_*` wrappers; `sys_getpid` now returns the leader pid |
| `syscall.c`, `syscall.h`, `usys.S` | dispatch + stubs for syscalls 22-26 |
| `user.h` | user-visible prototypes and `tid_t` typedef |
| `ttest.c` | smoke tests (create/join, ids, main thread_exit, exit-from-thread) |

## Locking notes

- `ptable.lock` guards any observation of thread state (`is_thread`,
  `thread_group`, `thread_count`, `state`, `killed`).
- `filedup`, `idup`, `fileclose`, `iput` must be called **without**
  holding `ptable.lock` — they may acquire buffer-cache sleep locks.
  `thread_create` does file/inode refcount work before acquiring the
  lock to set `RUNNABLE`; `thread_exit`/`exit` do it before acquiring
  the lock and then complete bookkeeping inside it.
- `sleep` chans in use:
  - `target` (a thread slot) — `thread_join` waits, `thread_exit`
    wakes.
  - `leader` (the group leader) — drain-for-image-swap waits,
    `thread_exit` and `exit` wake.
  - the parent proc — parent's `wait` waits, `thread_exit`/`exit`
    wake when `thread_count` reaches 0.

## Known limitations

- **Per-thread file descriptor tables**: not POSIX. A thread that
  opens an fd cannot be observed by another thread in the same
  process. Useful for teaching; replace with a shared fd table if
  you want POSIX-pthread semantics.
- **Stack is caller-provided and never freed**: `thread_create`
  trusts the caller to allocate and (after join) free the user stack.
  There is no guard page.
- **`NPROC = 64` is global**: threads consume process-table slots, so
  the system total cap is 64 including threads and processes.
- **No mutexes or condvars in the kernel API**: build them on top of
  this in user space (atomic CAS + yield, or a user-level sleep/wake
  if you add one).
- **`kill(tid)` delivers to the whole group**: there is no
  per-thread kill. This matches Linux's `kill(2)` (which targets a
  process), but differs from `pthread_kill`/`tgkill`.

## Testing

`ttest` covers:

1. create two threads, each incrementing a shared counter, then join —
   verifies the counter sum and the exit values round-trip.
2. `thread_getThreadId` / `thread_getProcessId` return distinct
   values; `getpid` agrees with `thread_getProcessId`.
3. main thread calls `thread_exit` with a sibling still alive — the
   parent's `wait()` only unblocks once the sibling also exits.
4. a thread calls `exit()` — the parent's `wait()` unblocks with the
   child's pid and every slot in the group is reaped.

Not covered by `ttest` but worth running by hand: image swap from a
non-main thread, `fork` from a non-main thread, `sbrk` stress, and
`usertests` as a regression suite to make sure the thread-aware
rewrites of `fork`/`exit`/`wait` didn't regress the single-threaded
path.
