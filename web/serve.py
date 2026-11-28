"""Serve only bundled BLE web assets on the loopback interface."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit
from urllib.request import build_opener, ProxyHandler
import argparse
import errno
import json
import socket
import webbrowser

ROOT = Path(__file__).resolve().parent
WEB_BUILD = 'CH592-LINK-0.5.0'


class LoopbackServer(ThreadingHTTPServer):
    allow_reuse_address = False

    def server_bind(self):
        if hasattr(socket, 'SO_EXCLUSIVEADDRUSE'):
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()
ASSETS = {
    '/': ('index.html', 'text/html; charset=utf-8'),
    '/index.html': ('index.html', 'text/html; charset=utf-8'),
    '/session.js': ('session.js', 'text/javascript; charset=utf-8'),
    '/core.js': ('core.js', 'text/javascript; charset=utf-8'),
    '/app.js': ('app.js', 'text/javascript; charset=utf-8'),
    '/style.css': ('style.css', 'text/css; charset=utf-8'),
}


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.headers.get('Host', '').split(':')[0].lower() not in ('localhost', '127.0.0.1'):
            self.send_error(403)
            return
        path = urlsplit(self.path).path
        if path == '/health.json':
            data = json.dumps({'tool': 'CH592-LINK-BLE', 'web_build': WEB_BUILD, 'protocol': 2}).encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.end_headers()
            self.wfile.write(data)
            return
        if path == '/favicon.ico':
            self.send_response(204)
            self.end_headers()
            return
        if path not in ASSETS:
            self.send_error(404)
            return
        filename, mime = ASSETS[path]
        data = (ROOT / filename).read_bytes()
        self.send_response(200)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'self'; style-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'")
        self.end_headers()
        self.wfile.write(data)


def compatible_server(port):
    try:
        with build_opener(ProxyHandler({})).open('http://127.0.0.1:%d/health.json' % port, timeout=0.5) as response:
            if response.status != 200:
                return False
            data = json.loads(response.read(2048))
        return data.get('tool') == 'CH592-LINK-BLE' and data.get('web_build') == WEB_BUILD
    except Exception:
        return False


def select_server(port, strict=False):
    ports = [0] if port == 0 else range(port, min(65536, port + (1 if strict else 10)))
    # Reuse a live same-build origin before creating an earlier free port.
    # Keeping one origin also preserves browser grants and same-origin locks.
    if port:
        for candidate in ports:
            if compatible_server(candidate):
                return None, candidate
    for candidate in ports:
        try:
            server = LoopbackServer(('127.0.0.1', candidate), Handler)
            return server, server.server_port
        except OSError as error:
            if error.errno not in (errno.EADDRINUSE, errno.EACCES) and getattr(error, 'winerror', 0) not in (10048, 10013):
                raise
            if compatible_server(candidate):
                return None, candidate
            if strict:
                raise
    raise OSError('No free loopback port; close an old tool server or choose --port.')


def main():
    parser = argparse.ArgumentParser(description='CH592 LINK BLE local HTML tool')
    parser.add_argument('--port', type=int, default=8593)
    parser.add_argument('--no-browser', action='store_true')
    parser.add_argument('--strict-port', action='store_true', help='Do not choose another port if this port is occupied.')
    args = parser.parse_args()
    if not 0 <= args.port <= 65535:
        parser.error('port must be 0..65535')
    print('Starting CH592 LINK web tool; checking local ports...', flush=True)
    server, port = select_server(args.port, args.strict_port)
    url = 'http://localhost:%d/' % port
    if server is None:
        print('Existing compatible CH592 LINK tool: ' + url, flush=True)
        if not args.no_browser:
            webbrowser.open(url)
        return
    with server:
        print('CH592 LINK BLE: ' + url, flush=True)
        print('Web build: ' + WEB_BUILD, flush=True)
        if args.port and port != args.port:
            print('Requested port is occupied by an older or different server; selected %d.' % port, flush=True)
        print('Open this URL in a Web Bluetooth capable Chrome or Edge. Ctrl+C stops this server.', flush=True)
        if not args.no_browser:
            webbrowser.open(url)
        try:
            server.serve_forever(poll_interval=0.3)
        except KeyboardInterrupt:
            print('\nServer stopped.')


if __name__ == '__main__':
    main()
