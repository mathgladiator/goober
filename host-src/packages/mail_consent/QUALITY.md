# mail_consent

Who a tenant may mail, and the proof (`design/machine/029.Invites.md` §§ 3–6): blocks, list
opt-ins and opt-outs with their evidence, the outreach ledger, and an append-only log.

## Quality bar

- **No addresses.** Every row is keyed by tenant and a hash of the address (the same hash the
  sent ledger and the old `unsubscribed.db` use). An opt-out can always be honored; a list of
  who opted out can't be produced.
- **Saying no always wins.** An opt-out is final for its list until the person opts in again
  themselves; an operator may record consent, never re-subscribe someone who said no. A block
  stops everything, and lifting it needs who and a statement.
- **Proof.** Every change is a `consent_log` row with when, how, and the evidence, kept five
  years; the current state is derived from it in the same transaction.
- **One file.** `storage/mail/consent.db`, opened once; every write is one transaction.
