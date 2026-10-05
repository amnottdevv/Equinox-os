# Multitasking — tasks, scheduler, consoles, pipes

Equinox OS is a preemptive, round-robin multitasking 32-bit kernel.
This page covers the task model; for the memory backing of tasks see
[MEMORY.md](MEMORY.md), for processes/spawn syscalls see
[SYSCALLS.md](SYSCALLS.md).

## Tasks

`struct Task` (kernel/library/header/task.h) is the schedulable unit.
Ring-0 tasks (shell, nettask, httpd) and per-program ring-3 tasks share
the same table.

- `MAX_TASKS` = **8** (fixed table);
- each task stores: id, name, state, register context, its own kernel
  stack, a per-task page directory (ring-3 tasks), an fd table, its
  cwd, saved args, and its zombie/exit status slot.

Lifecycle:

```
spawn / spawn2  →  RUNNING ⇄ BLOCKED (sleep/pipe/wait/io)
                        │
                        ▼
                     EXITED  ──(parent wait)──► reaped
```

A dead child becomes a **zombie**: its slot and exit status stay so the
parent can `wait` on it. Orphans are reaped when their parent dies;
when the table is full the oldest zombie is stolen.

## Scheduler

- Preemptive round-robin, **100 Hz** tick (`timer_init(100)`), quantum
  1 tick.
- FPU state preserved per task via `fxsave/fxrstor`.
- Ring-3 entry through a TSS-backed trampoline (user CS `0x1B`, DS
  `0x23`).
- When nothing is runnable the scheduler `hlt`s with interrupts on —
  idle is never a busy spin; sleeps are tick-precise.
- Context-switch paths take `task_sched_lock()`; storage and AHCI
  transfers run in **task context**, never in IRQ context (design
  invariant #2).

## Virtual consoles

Every task mirrors its own cell buffer + cursor. F1/F2 switch the
visible console; a background task that prints never corrupts the
active console's screen. A background task's first framebuffer draw
snapshots the text screen; on exit the text console is re-rendered.

## Pipes

`SYS_PIPE` creates a 4 KB kernel ring, ref-counted on both ends,
inherited across `spawn`. Read/write block on empty/full with EOF and
broken-pipe detection. The shell composes pipelines from these
primitives:

```sh
grep -n printf /test/hello.c | tr a-z A-Z > /mnt/out.txt
```

## Background jobs and nettask

Boot starts the **nettask** (kernel TCP/IP polling loop) and the httpd
as ordinary kernel tasks; the shell runs interactively on the main
task. `ps`, `kill`, `wait`, `switch`, `yield` are the user-facing
controls. Nested MRP runners inside one task are rejected
(`MRP_RUN_ERR_BUSY`) — wave-parallel builds (the `equinoxinstall`
compiler pool) use `task_create_user("mtcc-T%d", …)` workers instead.

## Limits summary

| Item | Value |
| --- | --- |
| Task slots | 8 |
| Quantum | 1 tick @ 100 Hz |
| Pipe ring | 4 KB |
| Consoles | one per task, F1/F2 visible |
| MRP nesting | 1 (BUSY on re-entry) |
