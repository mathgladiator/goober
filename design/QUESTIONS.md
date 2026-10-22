# Open Questions for a First Version

> Status: machine-authored and **not ratified**. The human answered the first round of
> questions in commit 2362f4d. Those answers, and the choices the human delegated, are
> recorded as decisions in `design/machine/005.FirstVersion.md`, so they are removed from
> here. What remains are the questions still open, plus new ones raised by building v1
> under `host-src/`. When one is answered, delete it here.

Every question has a gate:

- **F:x**: decide before feature x ships.
- **Later**: the human chose to explore it by playing.

## Still open from the first round

1. **Forced upgrades on a large database.** [Later] 420 rewrites each database in one transaction while its writes wait. What is the limit, and what happens to a large shared database? The human called this an experiment. *Cites:* `420.Migration.md` § Forcing an upgrade with a box.
2. **A fleet half-upgraded.** [Later] v1 is one host, and the human expects hosts to proxy for each other later. The question returns with the cluster. *Cites:* `420.Migration.md`; `400.Cluster.Service.md`.
3. **Diff kinds for approval.** [F: the operator UI] There is no schema kind and no upgrade-box kind. What makes a contraction never-waivable? *Cites:* `305.Approvals.md` § What an approval reads.
4. **Fields in prose but missing from 150.** [Later] The human wants to evolve the host organically. Each field gets ratified when the code reaches it. *Cites:* `150.DeepManifest.md`.
5. **Deferred protocols.** [Later] sse and the bus, mail receive, internal routes, box MCP routes, and uploads, worked on one at a time. *Cites:* `040`, `038`, `039`, `050`, `060`.

## New, from building v1

6. **The JSON request document vs the struct boundary.** [F: generators] v1 hands a box a JSON request document so boxes can be written by hand. `120.TheBoundary.md` designs typed structs, a pickup, and relocation. When do the generators replace the document, and does the JSON form stay as a fallback for one-shot boxes? *Cites:* `005.FirstVersion.md` §3; `120.TheBoundary.md` § The dance.
7. **Route-name grammar.** [Ratify] v1 uses `[a-z][a-z0-9_]*` for route names, as the human chose for names in general (Q10, Q13). `150.DeepManifest.md` § Rules every field obeys allows any identifier, like getForecast. Which one wins for chosen names? *Cites:* `005.FirstVersion.md` §5; CLAUDE.md § Manifest naming rule.
8. **Content tools vs the working set.** [F: other operators] v1's content_put writes straight to disk, and version_upload activates at once (the dev policy). `320.WorkingSet.md` and `305.Approvals.md` put drafts and approval in between. Should the operator UI bring both back at the same time? *Cites:* `005.FirstVersion.md` §9.
9. **Suspending boxes.** [F: async host calls] The human wants many boxes checked out at once, each blocked on an event without holding a thread. wasmtime supports this with async calls on fibers, and its C API exposes it (`wasmtime/async.h`). Should v2 move every box call to async, or only boxes whose manifest says `"boundary": "async"`? *Cites:* `005.FirstVersion.md` §2; `010.Manifest.md`.
10. **Box routes shadowing static content.** [Ratify] v1 answers a box route first, then static content. A file at the same path as a box route is silently unreachable. Should the checker refuse it, or warn as 019 does for shadowing? *Cites:* `005.FirstVersion.md` §8; `019.Conflicts.md`.
11. **Operator tokens vs host identities.** [F: OAuth] Identity now exists (006), but operator tokens are still separate bearer tokens acting as host:owner. When OAuth lands, should an agent operate the host by signing in as a host identity through the device flow, with minted tokens kept only for scripts? *Cites:* `006.LocalIdentity.md` §5; `013.Identity.md` § How an agent logs in.
12. **The domain registry in the config.** [F: domains] v1 reads domains from the host config, so adding one is an edit and a SIGHUP. `015.Domains.md` makes the registry a table with an API. Should the config seed the table, or stay the source of truth for a single host? *Cites:* `005.FirstVersion.md` §6; `015.Domains.md`.
13. **A realm per tenant or per domain.** [Ratify] v1 keys a realm by tenant, so two domains of one tenant share users. `013.Identity.md` makes every primary domain its own realm, with other domains redirecting. Is a realm per tenant acceptable until the registry of 015 exists? *Cites:* `006.LocalIdentity.md` §8; `013.Identity.md` § Identity is per domain.
14. **Re-granting the first owner.** [Ratify] v1 grants `owner` to the configured first owner whenever they sign in and nobody in the realm holds `owner`. That repairs an ownerless realm, but it also hands ownership back to the config's address after a deliberate transfer to someone who later left. Is that the right repair? *Cites:* `006.LocalIdentity.md` §3; `013.Identity.md` § Granting and revoking roles.
15. **Host sessions on tenant domains.** [F: operator dive] v1 lets an operator sign in as a host identity on any domain by ticking a box. 013 has them sign in on the platform domain and dive in, and Q49 chose a handshake. Is the shortcut acceptable for local play only? *Cites:* `006.LocalIdentity.md` §8.
16. **Grants from boxes.** [F: box-driven roles] `grantable` and `revocable` need a host call such as `goober_role_grant(role_ptr, role_len) -> i32`, applied only if the call succeeds. Should a grant be one more element of the write set, or wait for the WAL-backed write set of the data work? *Cites:* `013.Identity.md` § Granting and revoking roles; `005.FirstVersion.md` §11.
