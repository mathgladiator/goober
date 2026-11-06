# skills

Skills and playbooks (`design/machine/050.Skills.md`): what the host's engineers know how to do,
kept as one markdown file per skill, and the library the skill tools search.

## Quality bar

- **A SKILL.md.** Each file opens with an Agent Skills front matter (`name`, `description`), then
  the host's own fields, so it can be copied into any agent's skills folder as it is.
- **One form.** `check` refuses what a put can't keep (names by the Agent Skills rule, one-line
  titles and descriptions, 1 to 8 roles, a body of at most 64 KiB of UTF-8) and puts the rest in
  one form, so a file reads back to the same bytes (`to_file(from_file(f)) == f`).
- **Search a person can predict.** Every word of the query must match a word of the skill that
  starts with it; a name or title counts most, then roles, the description, and the body. No
  stemming, no synonyms.
- **Bounded.** At most 1000 skills; the host also budgets their bytes (`limits.skills-bytes`).
  Searches and reads share the library's lock, a write holds it alone, and the byte and skill
  totals are atomic, so a limits check never waits behind a search.
