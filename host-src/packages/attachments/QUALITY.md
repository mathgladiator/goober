# attachments

Numbered, immutable media per tenant: images and PDFs served at `/attachment/$id.$ext`
(`design/machine/007.Content.md` §7). A ledger in `storage/sqlite3/$tenant/internal.db`,
bytes under `storage/objects/$tenant/attachments/` through the host WAL.

## Quality bar

- **Types come from the bytes.** A name or a claimed type is never trusted; anything that
  isn't one of the known image types or PDF is refused, so an upload can't become a page.
- **The ledger knows every file.** The row is written as `writing` before the bytes, and
  set to `stored` or `failed` after, as 060.Files.md orders it.
- **Ids are never reused.** `AUTOINCREMENT`, and a deleted attachment keeps its row.
- **References are found by scanning text content** for `/attachment/$id`, so the unused
  report is exactly what a person reading the files would conclude.
