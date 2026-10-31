# ids

The host's id service (`design/human/021.PrimaryKeys.md`; `design/machine/038.Ids.md` § 2).

## Quality bar

- **Never vends twice.** The counter moves forward, and the range is recorded, in one durable
  transaction (`synchronous = FULL`) before any id of it is used. A crash loses ids, never
  repeats them.
- **Never goes back.** `move_past` only raises the counter; a new counter starts above 2^32
  and above any floor the host gives it.
- **Thread-safe.** One locked increment per id; ranges are taken under the same lock.
- **Tested:** ranges and restarts, eight threads drawing 40,000 ids with none shared, floors
  and restores, and `highest_id` over keys, AUTOINCREMENT's high-water mark, odd table names,
  and nested directories.
