#!/usr/bin/env python3
"""A tiny S3 stand-in for testing `sirius s3put` and the uploader: it checks the AWS Signature V4 of every PUT
independently of libcurl (own implementation of the algorithm) and stores the objects in a directory.

  mock_s3.py PORT STORE_DIR [--tls CERT KEY]

Credentials accepted: AKIATESTKEY / testsecret, region eu-north-1, bucket diary-test.
Object keys starting with "fail403/" get AccessDenied, "fail500/" a server error (the first N times, N = the digit
after the slash), so the retry paths can be tested.
"""
import hashlib, hmac, http.server, os, ssl, sys, urllib.parse

ACCESS, SECRET, REGION, BUCKET = "AKIATESTKEY", "testsecret", "eu-north-1", "diary-test"
failures = {}


def hmac_sha256(key, msg):
    return hmac.new(key, msg.encode(), hashlib.sha256).digest()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("mock_s3: " + fmt % args + "\n")

    def reply(self, status, body=b"", headers=()):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def error(self, status, code, msg):
        self.reply(status, f"<Error><Code>{code}</Code><Message>{msg}</Message></Error>".encode())

    def do_PUT(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("AWS4-HMAC-SHA256 "):
            return self.error(403, "AccessDenied", "no signature")
        fields = dict(p.strip().split("=", 1) for p in auth[len("AWS4-HMAC-SHA256 "):].split(","))
        access, date, region, service, term = fields["Credential"].split("/")
        signed = fields["SignedHeaders"].split(";")
        amz_date = self.headers["x-amz-date"]
        payload_hash = self.headers.get("x-amz-content-sha256", "")
        if payload_hash != hashlib.sha256(body).hexdigest():
            return self.error(400, "XAmzContentSHA256Mismatch", "payload hash differs")
        path, _, query = self.path.partition("?")
        canon_headers = "".join(f"{h}:{' '.join(self.headers[h].split())}\n" for h in signed)
        canon = "\n".join(["PUT", urllib.parse.quote(urllib.parse.unquote(path), safe="/-_.~"), query, canon_headers,
                           ";".join(signed), payload_hash])
        scope = f"{date}/{region}/{service}/aws4_request"
        to_sign = "\n".join(["AWS4-HMAC-SHA256", amz_date, scope, hashlib.sha256(canon.encode()).hexdigest()])
        k = hmac_sha256(("AWS4" + SECRET).encode(), date)
        for part in (region, service, "aws4_request"):
            k = hmac_sha256(k, part)
        want = hmac.new(k, to_sign.encode(), hashlib.sha256).hexdigest()
        if access != ACCESS or not hmac.compare_digest(want, fields["Signature"]):
            return self.error(403, "SignatureDoesNotMatch", "signature differs")
        if region != REGION or service != "s3":
            return self.error(403, "AuthorizationHeaderMalformed", "wrong region or service")
        parts = path.lstrip("/").split("/", 1)
        if len(parts) != 2 or parts[0] != BUCKET:
            return self.error(404, "NoSuchBucket", "no such bucket")
        key = urllib.parse.unquote(parts[1])
        if key.startswith("fail403/"):
            return self.error(403, "AccessDenied", "denied by test")
        if key.startswith("fail500/"):
            budget = int(key.split("/")[1][0])
            failures[key] = failures.get(key, 0) + 1
            if failures[key] <= budget:
                return self.error(500, "InternalError", "try again")
        dest = os.path.join(STORE, key)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(body)
        self.reply(200, headers=[("ETag", '"' + hashlib.md5(body).hexdigest() + '"')])


if __name__ == "__main__":
    port, STORE = int(sys.argv[1]), sys.argv[2]
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    if "--tls" in sys.argv:
        i = sys.argv.index("--tls")
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(sys.argv[i + 1], sys.argv[i + 2])
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    srv.serve_forever()
