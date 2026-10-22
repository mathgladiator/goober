# content

Static content per tenant, persisted under `storage/content/$tenant/` through the host WAL.
The `_host` tree holds host-wide content such as error pages.

## Quality bar

- Every write and delete goes through the WAL. Reads go straight to disk, so what an
  operator sees in the directory is exactly what is served.
- Tenant names and paths are validated on every call, reads included, so no request can
  reach outside its tenant's tree.
- Content types come only from the extension, from one table.
