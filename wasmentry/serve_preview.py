#!/usr/bin/env python3
"""Serve an unpacked preview at http://localhost:8000 with WASM thread headers."""
import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class PreviewHandler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'same-origin')
        super().end_headers()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8000)
    parser.add_argument('--directory', type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    handler = partial(PreviewHandler, directory=str(args.directory))
    ThreadingHTTPServer.request_queue_size = 128
    with ThreadingHTTPServer(('127.0.0.1', args.port), handler) as server:
        print(f'Magpie preview: http://localhost:{args.port}/wasmentry/', flush=True)
        server.serve_forever()
