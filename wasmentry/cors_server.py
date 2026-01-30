#!/usr/bin/env python3
"""
Simple HTTP server with CORS and SharedArrayBuffer support.

This server sets the required headers for enabling SharedArrayBuffer
in browsers, which is needed for pthread support in WebAssembly:
- Cross-Origin-Opener-Policy: same-origin
- Cross-Origin-Embedder-Policy: require-corp

Usage:
    python cors_server.py [port]

Default port is 8080.
"""

import sys
import http.server
import socketserver
from functools import partial


class CORSRequestHandler(http.server.SimpleHTTPRequestHandler):
    """HTTP request handler with CORS and SharedArrayBuffer headers."""

    def end_headers(self):
        # Required for SharedArrayBuffer (pthreads support)
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")

        # Standard CORS headers
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

        # Cache control for development
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")

        super().end_headers()

    def do_OPTIONS(self):
        """Handle preflight requests."""
        self.send_response(200)
        self.end_headers()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

    handler = partial(CORSRequestHandler, directory="..")

    # Enable socket reuse to avoid "Address already in use" errors
    socketserver.TCPServer.allow_reuse_address = True

    with socketserver.TCPServer(("", port), handler) as httpd:
        print(f"Server running at http://localhost:{port}/")
        print(f"Serving files from: .. (project root)")
        print()
        print("Headers enabled:")
        print("  ✓ Cross-Origin-Opener-Policy: same-origin")
        print("  ✓ Cross-Origin-Embedder-Policy: require-corp")
        print("  ✓ Access-Control-Allow-Origin: *")
        print()
        print("Open http://localhost:{port}/bin/test-worker.html to test")
        print("Press Ctrl+C to stop")
        print()

        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nServer stopped.")


if __name__ == "__main__":
    main()
