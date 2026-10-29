# What kcmeatup.com must serve after a deploy (Pit Boss, 2026-10-29). One readiness.py
# argument per line; set GOOBER_TOKEN for the --box checks.
--expect /places/rye-leawood text/html
--expect /sitemap.xml application/xml
--expect /calculate/meat-macros.csv text/csv
--box kc_locations
--box kc_access
