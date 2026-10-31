# What example.com must serve after a deploy: a template. A site's own list lives outside the
# repo, at ~/.config/goober/readiness.d/DOMAIN (or $GOOBER_READINESS_DIR/DOMAIN), on the machine
# that deploys. One readiness.py argument per line; set GOOBER_TOKEN for the --box checks.
--expect /places/some-place text/html
--expect /sitemap.xml application/xml
--box locations
