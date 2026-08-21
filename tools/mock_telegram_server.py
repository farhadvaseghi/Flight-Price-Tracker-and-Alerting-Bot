"""Stands in for Telegram's Bot API while testing the alerting layer.

Run it, then point the tracker at it:

    python tools/mock_telegram_server.py
    TELEGRAM_API_BASE=http://127.0.0.1:8099 \
    TELEGRAM_BOT_TOKEN=test TELEGRAM_CHAT_ID=@test ./flight_tracker --once

Paths mirror the real API. The token segment selects the behaviour:

  /bot<anything>/sendMessage  -> 200 {"ok": true}      normal success
  /botfail/sendMessage        -> 401 {"ok": false}     revoked token
  /botlimit/sendMessage       -> 429 twice with retry_after, then success
  /botbroken/sendMessage      -> 500                   server error
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

rate_limit_hits = {"count": 0}


class Handler(BaseHTTPRequestHandler):
    def _respond(self, code, body):
        payload = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        if code == 429:
            self.send_header("Retry-After", "0.4")
        self.end_headers()
        self.wfile.write(payload)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length)

        try:
            message = json.loads(raw)
        except ValueError:
            self._respond(400, {"ok": False, "description": "not JSON"})
            return

        token = self.path.split("/")[1][3:] if self.path.startswith("/bot") else ""
        print(f"  [telegram] chat={message.get('chat_id')} "
              f"parse_mode={message.get('parse_mode')} {length} bytes", flush=True)
        print("  " + "-" * 60, flush=True)
        print(message.get("text", ""), flush=True)
        print("  " + "-" * 60, flush=True)

        if token == "fail":
            self._respond(401, {"ok": False, "error_code": 401,
                                "description": "Unauthorized"})
        elif token == "limit":
            rate_limit_hits["count"] += 1
            if rate_limit_hits["count"] < 3:
                self._respond(429, {"ok": False, "error_code": 429,
                                    "description": "Too Many Requests: retry after 0",
                                    "parameters": {"retry_after": 0.4}})
            else:
                self._respond(200, {"ok": True, "result": {"message_id": 1}})
        elif token == "broken":
            self._respond(500, {"ok": False, "description": "Internal Server Error"})
        else:
            self._respond(200, {"ok": True, "result": {"message_id": 1}})

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    # The alert text contains emoji and arrows. A Windows console defaults to
    # cp1252, which cannot encode them, and print() would raise mid-request.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    print("mock Telegram API on http://127.0.0.1:8099", flush=True)
    HTTPServer(("127.0.0.1", 8099), Handler).serve_forever()
