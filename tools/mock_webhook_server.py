"""Tiny local server that echoes back whatever status code the path asks for.

Stands in for Discord while testing the webhook layer:
  POST /status/204  -> 204 No Content   (Discord's success code)
  POST /status/404  -> 404              (dead webhook)
  POST /status/500  -> 500              (server error, should retry)
  POST /status/429  -> 429 twice with Retry-After, then 204 (rate limit recovery)
"""
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

rate_limit_hits = {"count": 0}


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        print(f"  [server] POST {self.path} ({length} bytes) "
              f"content-type={self.headers.get('Content-Type')}", flush=True)

        code = int(self.path.rsplit("/", 1)[-1])

        # The 429 path succeeds on the third attempt, so the client's retry
        # loop can be observed actually recovering.
        if code == 429:
            rate_limit_hits["count"] += 1
            if rate_limit_hits["count"] < 3:
                payload = json.dumps(
                    {"message": "You are being rate limited.",
                     "retry_after": 0.4, "global": False}).encode()
                self.send_response(429)
                self.send_header("Content-Type", "application/json")
                self.send_header("Retry-After", "0.4")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                return
            code = 204

        if code == 204:
            self.send_response(204)
            self.end_headers()
            return

        payload = json.dumps({"message": f"synthetic {code}"}).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", 8099), Handler).serve_forever()
