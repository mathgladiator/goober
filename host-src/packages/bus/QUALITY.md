# bus

The in-process bus (`design/human/040.TheBus.md`, `design/machine/036.Bus.md` § 3): topics and
their sequence numbers, and who waits on each.

## Quality bar

- **Signals, never data**: a topic's state is one number.
- **Monotonic**: one counter for the process, seeded by the caller (the host seeds it with the
  time in microseconds), so a sequence never goes backwards, even across restarts.
- **Wakes outside the lock**: a watcher's callback runs after the bus's lock is released, so a
  callback may bump or watch without deadlock; callbacks must be quick.
- **Bounded**: a topic count limit, oldest-untouched topics forgotten first (a forgotten topic
  reads as 0, which only costs a wake).
