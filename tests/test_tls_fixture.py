#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The owned TLS fixture must contain valid, distinct trust chains and guarded cleanup."""
import importlib.util
import io
from pathlib import Path
import tempfile
import ssl
import socket
import unittest
from unittest.mock import patch
from cryptography import x509
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding

spec = importlib.util.spec_from_file_location('tls_fixture', Path(__file__).resolve().parents[1] / 'tools/run-tls-fixture.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)

class FixtureTests(unittest.TestCase):
    def test_distinct_roots_and_leaf_hostname(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            a, pem, _ = fixture.certificates(directory, 'trusted', '127.0.0.1')
            b, _, _ = fixture.certificates(directory, 'untrusted', '127.0.0.1')
            root = x509.load_der_x509_certificate(a)
            other = x509.load_der_x509_certificate(b)
            leaf = x509.load_pem_x509_certificate(pem.read_bytes())
            self.assertNotEqual(root.fingerprint(hashes.SHA256()), other.fingerprint(hashes.SHA256()))
            root.public_key().verify(leaf.signature, leaf.tbs_certificate_bytes, padding.PKCS1v15(), hashes.SHA256())
            with self.assertRaises(InvalidSignature):
                other.public_key().verify(leaf.signature, leaf.tbs_certificate_bytes, padding.PKCS1v15(), hashes.SHA256())
            self.assertEqual(str(leaf.extensions.get_extension_for_class(x509.SubjectAlternativeName).value[0].value), '127.0.0.1')

    def test_real_tls_accepts_one_root_and_refuses_the_other(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            a, pem, key = fixture.certificates(directory, 'trusted', '127.0.0.1')
            endpoints = [fixture.server('127.0.0.1', pem, key)]
            _, pem, key = fixture.certificates(directory, 'untrusted', '127.0.0.1')
            endpoints.append(fixture.server('127.0.0.1', pem, key))
            context = ssl.create_default_context(cadata=x509.load_der_x509_certificate(a).public_bytes(
                fixture.serialization.Encoding.PEM).decode())
            try:
                with socket.create_connection(('127.0.0.1', endpoints[0][0].server_port), timeout=3) as raw:
                    with context.wrap_socket(raw, server_hostname='127.0.0.1') as client:
                        client.sendall(b'GET / HTTP/1.0\r\n\r\n')
                        self.assertIn(b'200 OK', client.recv(1024))
                with socket.create_connection(('127.0.0.1', endpoints[1][0].server_port), timeout=3) as raw:
                    with self.assertRaises(ssl.SSLCertVerificationError):
                        context.wrap_socket(raw, server_hostname='127.0.0.1')
            finally:
                for endpoint, thread in endpoints:
                    endpoint.shutdown(); endpoint.server_close(); thread.join(timeout=3)

    def test_foreign_entry_refuses_cleanup(self):
        class FTP:
            def retrbinary(self, _, sink): sink(b'owned by somebody else\n')
        deleted = []
        with self.assertRaises(RuntimeError):
            fixture.remove_owned(FTP(), lambda *_: [], lambda *args: deleted.append(args))
        self.assertEqual(deleted, [])

    def test_unmarked_prefix_refuses_cleanup(self):
        deleted = []
        with self.assertRaises(RuntimeError):
            fixture.remove_owned(None, lambda _, path: None if path == fixture.GAME else [], lambda *args: deleted.append(args))
        self.assertEqual(deleted, [])

if __name__ == '__main__': unittest.main()
