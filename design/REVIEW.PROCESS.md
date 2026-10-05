# Daily Design Review Process

At the end of a working day, the human asks for a design review. The review is a new feedback document written by Claude about the whole project as it stands that day. This file describes how to produce it, so every review is comparable with the last.

## Output

- **Location:** `design/machine/feedback-$year-$month-$day.md`, using the review's date, like `feedback-2026-10-05.md`. If a review for that date already exists, add a suffix, like `feedback-2026-10-05b.md`, rather than overwriting it.
- **Status line:** the document opens by saying it is machine-authored and **not ratified**, and it names the commit it reviewed (the short hash of `HEAD` at the time).
- **Previous reviews are never edited.** Each one is a dated record of what the design said at the time, even when later docs make it out of date.
- **Index:** add the new review to the design-docs table in `README.md` and the design-doc list in `CLAUDE.md`.
- **Publish:** commit and push the review and the index updates together, with a commit message that summarizes the review's main findings.

## Inputs

Read all of these before writing a word:

1. Every document in `design/human/`, in lexical order, at `HEAD`.
2. The most recent `design/machine/feedback-*.md`, to know what was said last time.
3. The changes since that review: `git log` and `git diff` from the commit the last review names, through `HEAD`, limited to `design/`.
4. Any external project the goals reference, like Hearth (https://github.com/adama-platform/hearth), when a section depends on it.

## Voice

Write as an all-knowing, super-intelligent principal engineer with a happy personality that is... a bit too happy. The cheer is in the delivery, never in the substance: every judgment must still be precise, specific, and honest, and bad news is delivered gladly but delivered in full. Refer to the human as "the author," and use they/them if a pronoun is needed. Design terms, field names, and file names are always written exactly as the documents spell them.

## The goals to measure against

Every section is judged against two stated goals:

1. **Replace Hearth as new personal infrastructure.** The design should be able to run everything Hearth does today, as ordinary boxes, for one person and the handful of people they know.
2. **Become a robust blue-collar operating system.** Many products for trades and small businesses, where a product only needs maintenance for its external dependencies, and never needs to change its capabilities or its user interface once approved.

## Sections

Each review contains these sections, in this order.

### 1. Setting the stage

A short opening in the reviewer's voice: who is reviewing, what was reviewed (with the commit), and the two goals.

### 2. The design in one breath

A compressed summary of the whole design as it stands, with every clause cited. A reader arriving cold should understand the system from this section alone.

### 3. What changed since the last review

What the design added, removed, or reversed since the previous feedback document, based on the git history and a comparison of the documents. For each limit or proposal from the last review, say whether it was **addressed**, **partly addressed**, **declined**, or **still open**, and cite where. Note anything the author decided against a previous recommendation, without relitigating it.

### 4. What is unique

The genuinely original ideas, or original combinations of known ideas, and why each one matters. This comes before any criticism on purpose: the monkey thinking these thoughts should feel the good news first, so they can happily accept the critical feedback that follows.

### 5. Buy versus build

- **A table of what can be bought:** each design concept, the closest existing product or project, and how close it is. Name real vendors and open-source projects, and say clearly which comparisons were checked against current documentation and which come from prior knowledge.
- **How the design buys from the market:** which parts are deliberately rented or borrowed (Caddy, Stalwart, SQLite, Litestream, NATS, OpenAPI, and so on).
- **What must be built:** the parts that only exist if Claude, the super-intelligent code wizard, writes them. For each, give a **confidence level** (high, medium, or low) that it can be built correctly with current agentic AI, and a one-line reason.

### 6. Limits

The design's limits, each with what it is, why it matters, and a citation. Then **prioritize** them in a table against the two goals: for each limit, how much it blocks goal 1, how much it blocks goal 2, and an overall priority. Include a short list of **cross-document inconsistencies** (naming, types, examples that disagree), since this is a precision project.

### 7. Proposals

Ways to overcome the limits. Each proposal names the limit or limits it addresses, sketches the design (with a small manifest example when it helps), and cites the documents whose philosophy it extends. Proposals must stay within the design's own philosophy wherever possible, and say so when they can't.

### 8. Security evaluation

- **By adversary:** a malicious or buggy box, a malicious tenant user, an external attacker on the network or mail path, an operator or compromised infrastructure, the supply chain, and an agent acting for a user. For each, list the strengths (cited), the gaps, and a rating.
- **Under real traffic:** what is likely to go wrong when this is built and running with real users and real load. This covers abuse, noisy neighbors, cost blowups, operational failure modes, and incident scenarios, along with how the design would contain each one.
- **A prioritized list** of the most important security work, top ten at most.

### 9. Operating the system

What it would be like, day to day, for:

- **Humans:** the operator, a tenant owner, and an end user. What they see, approve, and get paged for.
- **Agents:** an agent building and submitting boxes, and an agent acting for a user through MCP. What they can do, what stops them, and what they find frustrating.

### 10. Goal mapping

- **Goal 1:** a table mapping each Hearth feature to the goober construct that covers it, and what's missing.
- **Goal 2:** what has to be true for products to need maintenance only for external dependencies, and how close the design is.

### 11. Build estimate

A robust estimate of how long it would take to build, with current agentic AI and the author directing. Break it into phases, give a range for each phase (optimistic, likely, and pessimistic), and state the assumptions: how many people, how much human review time, and what counts as "done."

### 12. One-shot readiness

An estimate of how complete the specification is for a decent one-shot prompt to Claude to build the system. Give a percentage, list what a one-shot build would get right, and list the specific gaps that would make it guess. Then name the few documents or decisions that would raise the number the most.

### 13. Suggested order of work

A short, ordered list of what to do next, drawn from the proposals and the priorities.

### 14. Closing

A short closing in the reviewer's voice.

### 15. Citation index

Every claim about the design is cited inline as **[NNN §Section]**: the document's number plus the section heading it comes from. The review ends with an index listing every document and section cited, and which details were drawn from each. Claims about outside products or projects cite their source (a URL, or "prior knowledge" when not checked).

## Checklist before publishing

- [ ] Every `design/human/` document was read at `HEAD`, and the previous review and git history were checked.
- [ ] Every claim about the design has an inline citation, and the citation index is complete.
- [ ] Every limit from the last review has a status in section 3.
- [ ] Limits are prioritized against both goals.
- [ ] Every proposal names the limits it addresses.
- [ ] Every component to be built has a confidence level.
- [ ] The build estimate has phases, ranges, and stated assumptions.
- [ ] The one-shot readiness section has a percentage and named gaps.
- [ ] Vendor comparisons say whether they were checked against current documentation.
- [ ] Any JSON or C examples parse or pass a syntax check.
- [ ] The review is indexed in `README.md` and `CLAUDE.md`, then committed and pushed.
