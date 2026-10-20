#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""src/pw_https.c against this PC's own TLS servers (Python's ssl as the oracle): what the title's downloads must take
and what they must refuse. Mbed TLS is built for the host from tools/setup-mbedtls.sh's pinned release."""
import datetime
import hashlib
import os
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

ROOT = Path(__file__).resolve().parents[1]
OUT = Path(os.environ.get('HOST_TEST_DIR', ROOT / '.deps/host-tests'))
BODY = os.urandom(3 << 20)
NAME = 'mihawkos.test'


def build():
    """The driver over a host build of Mbed TLS, rebuilt when its sources or ours change."""
    mbed = Path(subprocess.check_output(['bash', str(ROOT / 'tools/setup-mbedtls.sh')], text=True).strip())
    sources = sorted((mbed / 'library').glob('*.c'))
    ours = [ROOT / 'src/pw_https.c', ROOT / 'src/pw_sha256.c', ROOT / 'tests/pw_https_driver.c',
            ROOT / 'platform/tls/pw_mbedtls_config.h']
    stamp = hashlib.sha256(b''.join(p.read_bytes() for p in ours) + str(mbed).encode()).hexdigest()
    work = OUT / 'pw_https'
    driver = work / 'driver'
    if driver.exists() and (work / 'stamp').exists() and (work / 'stamp').read_text() == stamp:
        return driver
    work.mkdir(parents=True, exist_ok=True)
    flags = ['-O1', '-g', '-I', str(mbed / 'include'), '-I', str(mbed / 'library'), '-I', str(ROOT),
             '-DMBEDTLS_USER_CONFIG_FILE="platform/tls/pw_mbedtls_config.h"']
    objects = []
    jobs = []
    for source in sources:
        target = work / (source.stem + '.o')
        objects.append(str(target))
        jobs.append(subprocess.Popen([os.environ.get('CC', 'cc'), '-c', *flags, '-w', str(source), '-o', str(target)]))
    if any(job.wait() for job in jobs):
        raise RuntimeError('Mbed TLS did not build for the host')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', *flags, '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-o', str(driver), str(ROOT / 'tests/pw_https_driver.c'),
                    str(ROOT / 'src/pw_https.c'), str(ROOT / 'src/pw_sha256.c'), *objects], check=True)
    (work / 'stamp').write_text(stamp)
    return driver


def certificate(subject, issuer_key, issuer, key, start, end, ca=False, names=()):
    builder = (x509.CertificateBuilder().subject_name(subject).issuer_name(issuer).public_key(key.public_key())
               .serial_number(x509.random_serial_number()).not_valid_before(start).not_valid_after(end)
               .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
    if names:
        builder = builder.add_extension(x509.SubjectAlternativeName([x509.DNSName(n) for n in names]), critical=False)
    return builder.sign(issuer_key, hashes.SHA256())


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        port = self.server.server_address[1]
        if self.path == '/file':
            self.send_response(200)
            self.send_header('Content-Length', str(len(BODY)))
            self.end_headers()
            self.wfile.write(BODY)
        elif self.path == '/chunked':
            self.send_response(200)
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            for at in range(0, len(BODY), 70001):
                piece = BODY[at:at + 70001]
                self.wfile.write(f'{len(piece):x};ext=1\r\n'.encode() + piece + b'\r\n')
            self.wfile.write(b'0\r\nX-Trailer: yes\r\n\r\n')
        elif self.path == '/close':
            self.send_response(200)
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(BODY)
            self.close_connection = True
        elif self.path == '/relative':
            self.send_response(302)
            self.send_header('Location', '/file')
            self.send_header('Content-Length', '0')
            self.end_headers()
        elif self.path == '/absolute':
            self.send_response(301)
            self.send_header('Location', f'{self.server.scheme}://{NAME}:{port}/chunked')
            self.send_header('Content-Length', '0')
            self.end_headers()
        elif self.path == '/loop':
            self.send_response(307)
            self.send_header('Location', '/loop')
            self.send_header('Content-Length', '0')
            self.end_headers()
        else:
            self.send_response(404)
            self.send_header('Content-Length', '0')
            self.end_headers()


class Server(ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        pass   # a client that stops at its limit hangs up mid-body: expected here


class Https(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.driver = build()
        cls.work = tempfile.TemporaryDirectory()
        work = Path(cls.work.name)
        now = datetime.datetime.now(datetime.timezone.utc)
        day = datetime.timedelta(days=1)
        root_key, other_key = ec.generate_private_key(ec.SECP256R1()), ec.generate_private_key(ec.SECP256R1())
        root_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'MihawkOS test root')])
        other_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'Someone else')])
        root = certificate(root_name, root_key, root_name, root_key, now - day, now + 30 * day, ca=True)
        other = certificate(other_name, other_key, other_name, other_key, now - day, now + 30 * day, ca=True)
        (work / 'roots.pem').write_bytes(b'# a comment line, as in the console CA_LIST.cer\n' +
                                         root.public_bytes(serialization.Encoding.PEM))
        server_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, NAME)])
        kinds = {'good': (root_key, root_name, now - day, now + 30 * day, NAME),
                 'expired': (root_key, root_name, now - 10 * day, now - day, NAME),
                 'future': (root_key, root_name, now + day, now + 10 * day, NAME),
                 'wrongname': (root_key, root_name, now - day, now + 30 * day, 'other.test'),
                 'untrusted': (other_key, other_name, now - day, now + 30 * day, NAME)}
        cls.ports, cls.servers = {}, []
        for kind, (issuer_key, issuer, start, end, name) in kinds.items():
            key = ec.generate_private_key(ec.SECP256R1())
            leaf = certificate(server_name, issuer_key, issuer, key, start, end, names=(name,))
            (work / f'{kind}.pem').write_bytes(leaf.public_bytes(serialization.Encoding.PEM) + key.private_bytes(
                serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(work / f'{kind}.pem')
            server = Server(('127.0.0.1', 0), Handler)
            server.socket = context.wrap_socket(server.socket, server_side=True)
            server.scheme = 'https'
            cls.ports[kind] = server.server_address[1]
            cls.servers.append(server)
        plain = Server(('127.0.0.1', 0), Handler)
        plain.scheme = 'http'
        cls.ports['plain'] = plain.server_address[1]
        cls.servers.append(plain)
        for server in cls.servers:
            threading.Thread(target=server.serve_forever, daemon=True).start()
        cls.roots = str(work / 'roots.pem')

    @classmethod
    def tearDownClass(cls):
        for server in cls.servers:
            server.shutdown()
        cls.work.cleanup()

    def get(self, url, limit=1 << 30, roots=None):
        line = subprocess.run([str(self.driver), roots or self.roots, url, str(limit)], capture_output=True, text=True,
                              timeout=60, check=True).stdout
        return dict(item.split('=', 1) for item in line.split(' text=')[0].split())

    def url(self, kind, path):
        return f'{"http" if kind == "plain" else "https"}://{NAME}:{self.ports[kind]}{path}'

    def test_downloads(self):
        digest = hashlib.sha256(BODY).hexdigest()
        for kind, path, redirects in (('good', '/file', 0), ('good', '/chunked', 0), ('good', '/close', 0),
                                      ('good', '/relative', 1), ('good', '/absolute', 1), ('plain', '/file', 0)):
            with self.subTest(kind=kind, path=path):
                got = self.get(self.url(kind, path))
                self.assertEqual((got['status'], got['http'], got['bytes'], got['sha256'], got['redirects']),
                                 ('0', '200', str(len(BODY)), digest, str(redirects)))
        self.assertIn(self.get(self.url('good', '/file'))['entropy'], ('urandom', 'arandom'))

    def test_refusals(self):
        for kind, path, status in (('expired', '/file', '-6'), ('future', '/file', '-6'), ('wrongname', '/file', '-7'),
                                   ('untrusted', '/file', '-5'), ('good', '/missing', '-8'), ('good', '/loop', '-14')):
            with self.subTest(kind=kind, path=path):
                got = self.get(self.url(kind, path))
                self.assertEqual(got['status'], status)
                self.assertEqual(got['bytes'], '0')
        self.assertEqual(self.get(self.url('good', '/file'), limit=1000)['status'], '-11')
        self.assertEqual(self.get(self.url('good', '/chunked'), limit=1000)['status'], '-11')
        self.assertEqual(self.get('ftp://example/x')['status'], '-1')
        self.assertEqual(self.get(f'https://{NAME}:1/file')['status'], '-3')
        self.assertEqual(self.get(self.url('good', '/file'), roots='/nonexistent.pem')['status'], '-15')


if __name__ == '__main__':
    unittest.main()
