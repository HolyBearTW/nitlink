"""Serve the actual menu and offline fixtures on loopback only."""
import functools
import gzip
import http.server
import pathlib
import json
import re
import urllib.parse
from preview_game_bridge import dispatch, IMAGES
from artwork_processing import GENERATED

ROOT = pathlib.Path(__file__).resolve().parents[1]
class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass
    def do_GET(self):
        route=urllib.parse.urlsplit(self.path)
        if route.path=='/nitlink-menu.html':
            # A complete compressed response avoids partial HTML rewriting by
            # local HTTP filters. Decompressed bytes are the workspace document.
            body=(ROOT/'nitlink-menu.html').read_bytes()
            policy=re.search(rb'<meta http-equiv="Content-Security-Policy" content="([^"]+)"',body)
            body=gzip.compress(body,mtime=0)
            self.send_response(200)
            self.send_header('Content-Type','text/html; charset=utf-8')
            self.send_header('X-Content-Type-Options','nosniff')
            self.send_header('Cache-Control','no-store, no-transform')
            self.send_header('Content-Encoding','gzip')
            self.send_header('Content-Length',str(len(body)))
            self.send_header('Vary','Accept-Encoding')
            if policy:self.send_header('Content-Security-Policy',policy.group(1).decode('ascii'))
            self.close_connection=True
            try:
                self.end_headers()
                self.wfile.write(body)
            except (BrokenPipeError,ConnectionResetError):pass
            return
        if route.path=='/preview/bridge.js':
            if len(route.query)>16384 or self.headers.get('Host') not in ('127.0.0.1:8765','localhost:8765') or self.headers.get('Sec-Fetch-Site') in ('cross-site','same-site'):
                self.send_error(403); return
            params=urllib.parse.parse_qs(route.query)
            bridge_id=params.get('bridgeId',[''])[0]
            if not re.fullmatch(r'[1-9]\d{0,9}',bridge_id):
                self.send_error(400); return
            try:
                reply=dispatch(params)
            except Exception:
                reply={'status':'invalid'}
            # Fixed callback and JSON encoding, never API-supplied executable JS.
            body=('window.NitLinkPreviewBridge.deliver('+bridge_id+','+json.dumps(reply,ensure_ascii=True,allow_nan=False)+');').encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type','text/javascript; charset=utf-8')
            self.send_header('X-Content-Type-Options','nosniff')
            self.send_header('Cache-Control','no-store')
            self.send_header('Content-Length',str(len(body)))
            try:
                self.end_headers()
                self.wfile.write(body)
            except (BrokenPipeError,ConnectionResetError): pass
            return
        if route.path.startswith(('/preview/api-images/','/preview/generated-images/')):
            generated=route.path.startswith('/preview/generated-images/')
            name=route.path.rsplit('/',1)[1]
            if route.query or not re.fullmatch(r'[a-f0-9]{64}\.png',name):
                self.send_error(403); return
            try: body=((GENERATED if generated else IMAGES)/name).read_bytes()
            except OSError: self.send_error(404); return
            self.send_response(200)
            self.send_header('Content-Type','image/png')
            self.send_header('X-Content-Type-Options','nosniff')
            self.send_header('Content-Length',str(len(body)))
            try:
                self.end_headers()
                self.wfile.write(body)
            except (BrokenPipeError,ConnectionResetError): pass
            return
        super().do_GET()
    def end_headers(self):
        # Locale/script updates must become visible on a normal browser reload.
        if urllib.parse.urlsplit(self.path).path.startswith(('/locales/','/assets/menu/','/preview/fixtures.js')):
            self.send_header('Cache-Control','no-store, no-transform')
        super().end_headers()

if __name__ == '__main__':
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 8765), functools.partial(Handler, directory=str(ROOT)))
    print('NitLink fixture preview: http://127.0.0.1:8765/nitlink-menu.html?preview=games', flush=True)
    server.serve_forever()
