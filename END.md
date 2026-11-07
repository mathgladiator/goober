# The end of goober

Goober is going on hiatus. It was an experiment, and the experiment has given its results.
Over 34 days (2026-10-04 to 2026-11-07), it produced 297 commits, about 114,000 lines of C++ in
the host, 812 tests, and 61 machine design documents. Two agent teams, Claude and Grok, built
it and then built on it.

## What the experiment found

**The manifest idea was good, but agents had to work through it as a keyhole.** Declaring a box's
routes, data, and reach, and having the host enforce that, held up. But agents building through
manifests and MCP tools were working through a keyhole. Better tooling could widen that keyhole
somewhat, but it will not compete with a proper engineering harness: a repository, a test
runner, and an agent that can read and run things cheaply.

**The granularity was off.** Manifests grew big. One manifest carried routes, data, types, grants,
limits, and more, which made each one hard to read and hard to change. It may make more sense to
have data manifests that are independent of routing manifests, with pre- and postconditions
between them acting as the isolation barrier.

**The operations were a bright spot.** Running the host, watching it, controlling it, backing it
up, and reading what happened point to the future of this WebAssembly work. That part is worth
carrying forward.

**Data migrations and long-term thinking slow agents down.** Expand-only schemas, upgrade boxes,
and planning for every older version made each change heavy, and agents spent their effort
there instead of on the product.

**Beta with promotion to production came too late.** Beta with promotion to production turned
out to be the better idea, and constant approvals were the worse one. Agents need a model where
they work incrementally in a beta environment, and production deploys happen with a bulk review.
That may be the most critical part to get right.

## Why I am stopping

I felt out of control. A lot got built, and much of it was good, but I could no longer hold the
whole system in my head. Decisions piled up faster than I made them. The Grok team spent a
quarter of my token budget mostly moving things around, and I suspect much of it went to
unimportant work, because a chatbot does not have the right harness for building software.

## What happens next

This project will be harvested for good ideas. A new project will start with a new round of
human design documents. It will be time-boxed, so that I define the criteria for success clearly
up front and keep the feeling of control throughout.

Claude's thoughts on making the next version much better are in [`END.AI.md`](END.AI.md).
