# ranches

A directory of local ranches, farms, and butchers, an outreach log, and an index of what they
sell, read from their own websites by goober's polite robot (`design/human/800.Crawl.md`,
`design/machine/017.Crawl.md`). Its data lives in the fixed tenant `meat`, so every domain
the box serves shares one index.

## Use it

- An agent adds ranches with `ranch_add` (give the website), logs every email, call, or visit
  with `contact_log` (which moves the status along: prospect, contacted, replied, interested,
  partner, declined), and reads them back with `ranch_list` and `ranch_get`.
- Every five minutes a timer reads the watched website read longest ago, if a week has
  passed: schema.org `Product` and `Offer` data, a Shopify store's `/products.json`, or the
  ranch's own `extract` spec (try one with the operator's `web_fetch` first). `ranch_check`
  does it now.
- `offering_search` finds cuts across every ranch; `/meat/offerings?q=brisket&category=beef`
  and `/meat/ranches` are public (CORS open), without notes, emails, or the contact log.
- `offering_add` records what the robot can't read: a price from a call or a market stall.

Finding ranches in the first place is the agent's job (a search engine, a map, a farmers
market list); this box is where what it finds is kept.
