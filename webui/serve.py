#!/usr/bin/env python3
"""Serve the MAGPIE web UI with the headers WASM threads require.

Serves the repository root (so /webui, /wasmentry, and /data all resolve)
regardless of the directory this is launched from, and sends the
Cross-Origin-Opener-Policy / Cross-Origin-Embedder-Policy headers needed for
SharedArrayBuffer (pthreads).

Usage:
    python3 webui/serve.py [port]      # default port 8080
Then open http://localhost:8080/webui/
"""

import http.server
import os
import socket
import socketserver
import sys
from functools import partial

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class CORSRequestHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(200)
        self.end_headers()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    handler = partial(CORSRequestHandler, directory=REPO_ROOT)
    socketserver.TCPServer.allow_reuse_address = True
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.connect(("8.8.8.8", 80))
        local_ip = sock.getsockname()[0]
        sock.close()
    except OSError:
        local_ip = "localhost"

    with socketserver.TCPServer(("0.0.0.0", port), handler) as httpd:
        print(f"Serving {REPO_ROOT}")
        print(f"  Desktop: http://localhost:{port}/webui/")
        print(f"  LAN:     http://{local_ip}:{port}/webui/")
        print("COOP/COEP enabled (SharedArrayBuffer ready). Ctrl+C to stop.")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nStopped.")


if __name__ == "__main__":
    main()
