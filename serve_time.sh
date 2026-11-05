#!/usr/bin/env bash
# 在笔记本上运行：极简 HTTP 时间源，供载板 ./sync_time.sh 取时间
# 用法：./serve_time.sh [port] [bind_addr]
set -euo pipefail

PORT="${1:-8899}"
BIND="${2:-192.168.137.1}"

exec python3 - "${PORT}" "${BIND}" <<'PY'
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = f"{int(time.time())}\n".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


port, bind = int(sys.argv[1]), sys.argv[2]
print(f"time source on http://{bind}:{port}/", flush=True)
ThreadingHTTPServer((bind, port), Handler).serve_forever()
PY
