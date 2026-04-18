# Add Threads To xv6

## Background
In this exercise you will be adding threads to the xv6 OS.

## API - User Mode Syscalls

- void thread_create(void *entry(void), void *stack, uint stack_size): 
        Create a new thread.
        The new thread will start at @param entry with stack with size @param stack_size at @param stack.
        Both @param entry and @param stack are virtual addresses in the user adress space.

- void thread_exit(void *exit_value):
        Kills the current thread. If performed by the main thread, should not terminate the entire process.
        The @param exit_value is the exit value to be stored and returned if a different thread joins the exited thread (see: thread_join).

- void thread_join(uint tid, void *exit_value):
        Halts the thread execution until the thread with thread id @param tid.
        The @param exit_value will hold the thread with @param tid exit value.

- uint thread_getThreadId():
        Returns current thread's tid.

- uint thread_getProcessId():
        Returns the main thread's pid.

## Edge Cases
In any edge case (like performing `exec`, `fork`, or `exit` from a thread) xv6 should act as mainline Linux (for example, `exec` inside a thread, even if not the main thread, will kill all other threads in the process).
