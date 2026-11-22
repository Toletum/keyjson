import json
import threading
import unittest
import urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import cast
from unittest.mock import patch
from urllib.parse import unquote_to_bytes, urlsplit

from keyjson import (
    JSONValue,
    KeyJsonClient,
    KeyJsonHTTPError,
    KeyJsonProtocolError,
    KeyJsonTransportError,
    KeyNotFoundError,
)


class _TestServer(ThreadingHTTPServer):
    daemon_threads = True
    store: dict[str, object]
    counts: dict[str, int]
    response_mode: str | None
    ttl_headers: dict[str, str | None]


class _Handler(BaseHTTPRequestHandler):
    @property
    def _test_server(self) -> _TestServer:
        return cast(_TestServer, self.server)

    def log_message(self, format, *args):
        pass

    def _send_raw(self, status, body, content_type="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, status, payload, content_type="application/json"):
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
        self._send_raw(status, body.encode("utf-8"), content_type)

    def _maybe_override(self):
        mode = self._test_server.response_mode
        if mode == "invalid-json":
            self._send_raw(200, b"{", "application/json")
        elif mode == "wrong-content-type":
            self._send_raw(200, b"{}", "text/plain")
        elif mode == "http-error":
            self._send_json(418, {"error": "teapot"})
        elif mode == "invalid-stats":
            self._send_json(200, {"keys": "not-an-integer"})
        else:
            return False
        return True

    def _key_from_path(self):
        path = urlsplit(self.path).path
        prefix = "/keys/"
        if not path.startswith(prefix):
            return None
        try:
            return unquote_to_bytes(path[len(prefix):]).decode("utf-8")
        except UnicodeDecodeError:
            return None

    def _read_json_body(self):
        body_length = int(self.headers.get("Content-Length", "0"))
        return json.loads(self.rfile.read(body_length).decode("utf-8"))

    def do_PUT(self):
        key = self._key_from_path()
        if key is None:
            self._send_json(400, {"error": "invalid_key"})
            return
        try:
            value = self._read_json_body()
        except (UnicodeDecodeError, json.JSONDecodeError):
            self._send_json(400, {"error": "invalid_json"})
            return

        store = self._test_server.store
        created = key not in store
        store[key] = value
        self._test_server.ttl_headers[key] = self.headers.get("X-TTL-Seconds")
        self._test_server.counts["sets"] += 1
        self._send_json(201 if created else 200, {"created": created})

    def do_GET(self):
        if self._maybe_override():
            return
        path = urlsplit(self.path).path
        if path == "/keys":
            self._send_json(200, list(self._test_server.store))
            return
        if path == "/stats":
            stored_bytes = sum(
                len(key.encode("utf-8"))
                + len(
                    json.dumps(value, ensure_ascii=False, separators=(",", ":"))
                    .encode("utf-8")
                )
                for key, value in self._test_server.store.items()
            )
            self._send_json(
                200,
                {
                    "keys": len(self._test_server.store),
                    "capacity": max(16, len(self._test_server.store) * 2),
                    "stored_bytes": stored_bytes,
                    **self._test_server.counts,
                },
            )
            return

        key = self._key_from_path()
        if key is None or key not in self._test_server.store:
            self._send_json(404, {"error": "key_not_found"})
            return
        self._test_server.counts["gets"] += 1
        self._send_json(200, self._test_server.store[key])

    def do_DELETE(self):
        key = self._key_from_path()
        if key is None or key not in self._test_server.store:
            self._send_json(404, {"error": "key_not_found"})
            return
        del self._test_server.store[key]
        self._test_server.counts["deletes"] += 1
        self._send_json(200, {"deleted": True})


class KeyJsonClientTests(unittest.TestCase):
    server: _TestServer
    thread: threading.Thread
    base_url: str

    @classmethod
    def setUpClass(cls):
        cls.server = _TestServer(("127.0.0.1", 0), _Handler)
        cls.server.store = {}
        cls.server.counts = {"sets": 0, "gets": 0, "deletes": 0}
        cls.server.response_mode = None
        cls.server.ttl_headers = {}
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        port = cls.server.server_address[1]
        cls.base_url = f"http://127.0.0.1:{port}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=3)

    def setUp(self):
        self.server.store.clear()
        self.server.counts = {"sets": 0, "gets": 0, "deletes": 0}
        self.server.response_mode = None
        self.server.ttl_headers.clear()
        self.client = KeyJsonClient(self.base_url, timeout=1.0)

    def test_crud_json_values_and_url_escaping(self):
        key = "space / 雪?#"
        value = {"message": "hello", "items": [1, True, None, {"x": 2.5}]}

        self.assertTrue(self.client.put(key, value))
        self.assertEqual(value, self.client.get(key))
        self.assertFalse(self.client.put(key, "replacement"))
        self.assertEqual("replacement", self.client.get(key))
        self.client.delete(key)
        with self.assertRaises(KeyNotFoundError):
            self.client.get(key)

    def test_put_ttl_header_and_clear_on_update_without_ttl(self):
        self.assertTrue(self.client.put("temporary", "value", ttl=30))
        self.assertEqual("30", self.server.ttl_headers["temporary"])

        self.assertFalse(self.client.put("temporary", "permanent"))
        self.assertIsNone(self.server.ttl_headers["temporary"])

    def test_json_null_round_trips(self):
        self.assertTrue(self.client.put("null", None))
        self.assertIsNone(self.client.get("null"))

    def test_list_and_stats(self):
        self.client.put("alpha", {"n": 1})
        self.client.put("beta", [2, 3])

        self.assertEqual({"alpha", "beta"}, set(self.client.keys()))
        stats = self.client.stats()
        self.assertEqual(2, stats["keys"])
        self.assertEqual(2, stats["sets"])
        stored_bytes = stats["stored_bytes"]
        self.assertIsInstance(stored_bytes, int)
        if isinstance(stored_bytes, int):
            self.assertGreater(stored_bytes, 0)

    def test_not_found_for_get_and_delete(self):
        with self.assertRaises(KeyNotFoundError) as error:
            self.client.get("missing")
        self.assertEqual(404, error.exception.status_code)
        self.assertEqual("missing", error.exception.key)

        with self.assertRaises(KeyNotFoundError):
            self.client.delete("missing")

    def test_http_errors_preserve_status_and_error_code(self):
        self.server.response_mode = "http-error"
        with self.assertRaises(KeyJsonHTTPError) as error:
            self.client.stats()
        self.assertEqual(418, error.exception.status_code)
        self.assertEqual("teapot", error.exception.error_code)

    def test_malformed_and_unexpected_responses_raise_protocol_error(self):
        for mode in ("invalid-json", "wrong-content-type", "invalid-stats"):
            with self.subTest(mode=mode):
                self.server.response_mode = mode
                with self.assertRaises(KeyJsonProtocolError):
                    self.client.stats()

    def test_transport_errors_are_wrapped(self):
        with (
            patch(
                "urllib.request.urlopen",
                side_effect=urllib.error.URLError("offline"),
            ),
            self.assertRaises(KeyJsonTransportError),
        ):
            self.client.stats()

    def test_invalid_keys_and_values_are_rejected_locally(self):
        for key in ("", "x" * 1025, "control\nkey", "\ud800"):
            with self.subTest(key=repr(key)), self.assertRaises(ValueError):
                self.client.get(key)
        with self.assertRaises(TypeError):
            self.client.get(cast(str, None))
        with self.assertRaises(ValueError):
            self.client.put("nan", float("nan"))
        with self.assertRaises(ValueError):
            self.client.put("unsupported", cast(JSONValue, object()))
        with self.assertRaises(ValueError):
            self.client.put("bad-ttl", "value", ttl=0)
        with self.assertRaises(ValueError):
            self.client.put("bad-ttl", "value", ttl=-1)
        with self.assertRaises(TypeError):
            self.client.put("bad-ttl", "value", ttl=cast(int, 1.5))
        with self.assertRaises(TypeError):
            self.client.put("bad-ttl", "value", ttl=True)

    def test_invalid_client_configuration(self):
        for url in (
            "ftp://localhost",
            "http://user:password@localhost",
            "http://localhost?query=1",
        ):
            with self.subTest(url=url), self.assertRaises(ValueError):
                KeyJsonClient(url)
        with self.assertRaises(ValueError):
            KeyJsonClient(self.base_url, timeout=0)


if __name__ == "__main__":
    unittest.main()
