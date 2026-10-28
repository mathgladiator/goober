#!/usr/bin/env python3
"""A stand-in for S3 (and SES) on 127.0.0.1, for trying backups without AWS (stdlib only).

    tools/fake_s3.py [--port 9000] [--dir DIR] [--access-key K --secret S]

Path-style buckets, kept as files under DIR (default: a temporary directory). Every
request's Signature Version 4 is checked with this file's own implementation, written
apart from the host's, so the two keep each other honest. SES's SendEmail is accepted
and each message written to DIR/_mail/. Point a host at it with backup.s3.endpoint
"http://127.0.0.1:9000" (and identity.ses.endpoint for SES), and set the AWS keys to the
same pair.
"""

import argparse
import base64
import hashlib
import hmac
import http.server
import json
import pathlib
import sys
import tempfile
import urllib.parse


def sign(key, msg):
    return hmac.new(key, msg.encode(), hashlib.sha256).digest()


def expected_signature(method, path, query, headers, signed, payload_hash, secret, date, region, service):
    pairs = sorted((urllib.parse.quote(k, safe="-_.~"), urllib.parse.quote(v, safe="-_.~"))
                   for k, v in urllib.parse.parse_qsl(query, keep_blank_values=True))
    canonical_query = "&".join(f"{k}={v}" for k, v in pairs)
    names = signed.split(";")
    canonical_headers = "".join(f"{n}:{' '.join(headers.get(n, '').split())}\n" for n in names)
    canonical = "\n".join([method, path, canonical_query, canonical_headers, signed, payload_hash])
    scope = f"{date[:8]}/{region}/{service}/aws4_request"
    to_sign = "\n".join(["AWS4-HMAC-SHA256", date, scope, hashlib.sha256(canonical.encode()).hexdigest()])
    k = sign(("AWS4" + secret).encode(), date[:8])
    for part in (region, service, "aws4_request"):
        k = sign(k, part)
    return hmac.new(k, to_sign.encode(), hashlib.sha256).hexdigest()


class Handler(http.server.BaseHTTPRequestHandler):
    root = None
    access_key = None
    secret = None
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, status, body=b"", ctype="application/xml"):
        self.send_response(status)
        self.send_header("content-type", ctype)
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def error(self, status, code):
        self.reply(status, f"<Error><Code>{code}</Code><Message>{code}</Message></Error>".encode())

    def body(self):
        if self.headers.get("expect", "").lower() == "100-continue":
            self.send_response_only(100)
            self.end_headers()
        n = int(self.headers.get("content-length", "0") or 0)
        return self.rfile.read(n) if n else b""

    def checked(self, data):
        auth = self.headers.get("authorization", "")
        if not auth.startswith("AWS4-HMAC-SHA256 "):
            return "no SigV4 authorization"
        fields = dict(p.strip().split("=", 1) for p in auth[17:].split(","))
        key_id, date8, region, service, _ = fields["Credential"].split("/")
        if key_id != self.access_key:
            return "InvalidAccessKeyId"
        date = self.headers.get("x-amz-date", "")
        payload = self.headers.get("x-amz-content-sha256") or hashlib.sha256(data).hexdigest()
        if payload != "UNSIGNED-PAYLOAD" and payload != hashlib.sha256(data).hexdigest():
            return "XAmzContentSHA256Mismatch"
        raw_path, _, query = self.path.partition("?")
        headers = {k.lower(): v for k, v in self.headers.items()}
        want = expected_signature(self.command, raw_path, query, headers, fields["SignedHeaders"], payload, self.secret,
                                  date, region, service)
        return "" if hmac.compare_digest(want, fields["Signature"]) else "SignatureDoesNotMatch"

    def handle_any(self):
        data = self.body() if self.command in ("PUT", "POST") else b""
        why = self.checked(data)
        if why:
            return self.error(403, why)
        raw_path, _, query = self.path.partition("?")
        if self.command == "POST" and raw_path == "/v2/email/outbound-emails":
            message = json.loads(data)
            raw = base64.b64decode(message["Content"]["Raw"]["Data"])
            box = self.root / "_mail"
            box.mkdir(exist_ok=True)
            n = len(list(box.iterdir())) + 1
            (box / f"{n}.eml").write_bytes(raw)
            return self.reply(200, json.dumps({"MessageId": f"fake-{n}"}).encode(), "application/json")
        path = urllib.parse.unquote(raw_path)
        # The path as AWS would canonicalize it, worked out here on its own: a client that
        # encodes keys differently from S3 fails here, not first against real AWS.
        if urllib.parse.quote(path, safe="/-_.~") != raw_path:
            return self.error(400, "PathNotCanonical")
        bucket, _, key = path.lstrip("/").partition("/")
        if not bucket or ".." in path.split("/") or "." in path.split("/"):
            return self.error(400, "InvalidRequest")
        base = self.root / bucket
        if self.command == "GET" and not key:
            q = dict(urllib.parse.parse_qsl(query))
            prefix, token = q.get("prefix", ""), q.get("continuation-token", "")
            keys = sorted(str(p.relative_to(base)) for p in base.rglob("*") if p.is_file()) if base.exists() else []
            keys = [k for k in keys if k.startswith(prefix) and k > token]
            page, more = keys[:1000], len(keys) > 1000
            items = "".join(f"<Contents><Key>{urllib.parse.quote(k)}</Key><Size>{(base / k).stat().st_size}</Size>"
                            f"<LastModified>2026-01-01T00:00:00.000Z</LastModified></Contents>" for k in page)
            tail = f"<IsTruncated>true</IsTruncated><NextContinuationToken>{page[-1]}</NextContinuationToken>" if more else ""
            return self.reply(200, f"<ListBucketResult>{items}{tail}</ListBucketResult>".encode())
        target = base / key
        if self.command == "PUT":
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            return self.reply(200)
        if self.command == "GET":
            return self.reply(200, target.read_bytes(), "application/octet-stream") if target.is_file() else self.error(404, "NoSuchKey")
        if self.command == "DELETE":
            target.unlink(missing_ok=True)
            return self.reply(204)
        return self.error(405, "MethodNotAllowed")

    do_GET = do_PUT = do_POST = do_DELETE = handle_any


def serve(port, root, access_key, secret):
    Handler.root, Handler.access_key, Handler.secret = pathlib.Path(root), access_key, secret
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    return server


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=9000)
    p.add_argument("--dir")
    p.add_argument("--access-key", default="AKIDGOOBERFAKE")
    p.add_argument("--secret", default="fake-secret")
    a = p.parse_args()
    root = a.dir or tempfile.mkdtemp(prefix="fake-s3-")
    server = serve(a.port, root, a.access_key, a.secret)
    print(f"fake S3 on http://127.0.0.1:{server.server_address[1]}, keeping objects in {root}", file=sys.stderr, flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
