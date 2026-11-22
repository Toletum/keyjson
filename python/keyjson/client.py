"""Standard-library client for the keyjson REST API."""


import http.client
import json
import math
import typing
import urllib.error
import urllib.parse
import urllib.request


JSONValue = str | int | float | bool | list["JSONValue"] | dict[str, "JSONValue"] | None

_MAX_KEY_BYTES = 1024
_STATS_FIELDS = ("keys", "capacity", "stored_bytes", "sets", "gets", "deletes")


class KeyJsonError(Exception):
    """Base exception for keyjson client failures."""


class KeyJsonHTTPError(KeyJsonError):
    """An HTTP error response returned by the keyjson server."""

    def __init__(self, status_code: int, error_code: str | None = None) -> None:
        self.status_code = status_code
        self.error_code = error_code
        detail = error_code or "unexpected HTTP response"
        super().__init__(f"keyjson returned HTTP {status_code}: {detail}")


class KeyNotFoundError(KeyJsonHTTPError):
    """Raised when a requested key does not exist."""

    def __init__(self, key: str) -> None:
        self.key = key
        super().__init__(404, "key_not_found")
        self.args = (f"Key {key!r} was not found",)


class KeyJsonTransportError(KeyJsonError):
    """A network or HTTP transport failure."""


class KeyJsonProtocolError(KeyJsonError):
    """A malformed or unexpected response from the server."""


class KeyJsonClient:
    """A small synchronous client for a keyjson server."""

    def __init__(
        self,
        base_url: str = "http://127.0.0.1:8080",
        timeout: float = 5.0,
    ) -> None:
        if not isinstance(base_url, str):
            raise TypeError("base_url must be a string")
        parsed = urllib.parse.urlsplit(base_url)
        if parsed.scheme not in ("http", "https") or not parsed.netloc:
            raise ValueError("base_url must be an absolute HTTP or HTTPS URL")
        if parsed.username is not None or parsed.password is not None:
            raise ValueError("base_url must not contain credentials")
        if parsed.query or parsed.fragment:
            raise ValueError("base_url must not contain a query or fragment")
        if isinstance(timeout, bool) or not isinstance(timeout, (int, float)):
            raise TypeError("timeout must be a positive number")
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("timeout must be a positive finite number")

        self.base_url = base_url.rstrip("/")
        self.timeout = float(timeout)
        self._scheme = parsed.scheme
        self._netloc = parsed.netloc
        self._path_prefix = parsed.path.rstrip("/")

    @staticmethod
    def _encode_key(key: str) -> str:
        if not isinstance(key, str):
            raise TypeError("key must be a string")
        try:
            encoded = key.encode("utf-8")
        except UnicodeEncodeError as error:
            raise ValueError("key must be valid UTF-8") from error
        if not encoded or len(encoded) > _MAX_KEY_BYTES:
            raise ValueError("key must contain between 1 and 1024 UTF-8 bytes")
        if any(byte < 0x20 or byte == 0x7F for byte in encoded):
            raise ValueError("key must not contain control characters")
        return urllib.parse.quote(key, safe="")

    @staticmethod
    def _encode_json(value: JSONValue) -> bytes:
        try:
            serialized = json.dumps(
                value,
                ensure_ascii=False,
                allow_nan=False,
                separators=(",", ":"),
            )
            return serialized.encode("utf-8")
        except (TypeError, ValueError, UnicodeEncodeError) as error:
            raise ValueError("value must be serializable as valid JSON") from error

    @staticmethod
    def _reject_json_constant(value: str) -> None:
        raise ValueError(f"invalid JSON constant: {value}")

    @classmethod
    def _decode_json(cls, body: bytes) -> object:
        try:
            text = body.decode("utf-8")
            return json.loads(text, parse_constant=cls._reject_json_constant)
        except (UnicodeDecodeError, ValueError) as error:
            raise KeyJsonProtocolError("server returned invalid JSON") from error

    @classmethod
    def _error_code(cls, body: bytes) -> str | None:
        try:
            payload = cls._decode_json(body)
        except KeyJsonProtocolError:
            return None
        if isinstance(payload, dict) and isinstance(payload.get("error"), str):
            return payload["error"]
        return None

    def _url(self, route: str) -> str:
        path = f"{self._path_prefix}{route}"
        return urllib.parse.urlunsplit((self._scheme, self._netloc, path, "", ""))

    def _decode_response(self, body: bytes, content_type: str) -> object:
        media_type = content_type.split(";", 1)[0].strip().lower()
        if media_type != "application/json":
            raise KeyJsonProtocolError(
                "server response Content-Type is not application/json"
            )
        if not body:
            raise KeyJsonProtocolError("server returned an empty JSON response")
        return self._decode_json(body)

    def _request(
        self,
        method: str,
        route: str,
        body: bytes | None = None,
        not_found_key: str | None = None,
        extra_headers: dict[str, str] | None = None,
    ) -> tuple[int, object]:
        headers = {"Accept": "application/json"}
        if body is not None:
            headers["Content-Type"] = "application/json"
        if extra_headers is not None:
            headers.update(extra_headers)
        request = urllib.request.Request(
            self._url(route), data=body, headers=headers, method=method
        )

        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                status = response.getcode()
                content_type = response.headers.get("Content-Type", "")
                response_body = response.read()
        except urllib.error.HTTPError as error:
            try:
                error_body = error.read()
            finally:
                error.close()
            if error.code == 404 and not_found_key is not None:
                raise KeyNotFoundError(not_found_key) from error
            raise KeyJsonHTTPError(
                error.code, self._error_code(error_body)
            ) from error
        except (
            urllib.error.URLError,
            http.client.HTTPException,
            OSError,
        ) as error:
            raise KeyJsonTransportError(f"keyjson request failed: {error}") from error

        return status, self._decode_response(response_body, content_type)

    def put(
        self,
        key: str,
        value: JSONValue,
        *,
        ttl: int | None = None,
    ) -> bool:
        """Create or replace a key, optionally expiring after ttl seconds."""
        encoded_key = self._encode_key(key)
        body = self._encode_json(value)
        extra_headers = None
        if ttl is not None:
            if isinstance(ttl, bool) or not isinstance(ttl, int):
                raise TypeError("ttl must be a positive integer number of seconds")
            if ttl <= 0:
                raise ValueError("ttl must be greater than zero")
            extra_headers = {"X-TTL-Seconds": str(ttl)}
        status, response = self._request(
            "PUT",
            f"/keys/{encoded_key}",
            body=body,
            extra_headers=extra_headers,
        )
        if (
            status not in (200, 201)
            or not isinstance(response, dict)
            or type(response.get("created")) is not bool
        ):
            raise KeyJsonProtocolError("unexpected response to PUT")
        created = response["created"]
        if (status == 201) != created:
            raise KeyJsonProtocolError("PUT status does not match created result")
        return created

    def get(self, key: str) -> JSONValue:
        """Return a key's JSON value or raise KeyNotFoundError."""
        encoded_key = self._encode_key(key)
        _, response = self._request(
            "GET", f"/keys/{encoded_key}", not_found_key=key
        )
        return typing.cast(JSONValue, response)

    def delete(self, key: str) -> None:
        """Delete a key or raise KeyNotFoundError if it does not exist."""
        encoded_key = self._encode_key(key)
        status, response = self._request(
            "DELETE", f"/keys/{encoded_key}", not_found_key=key
        )
        if (
            status != 200
            or not isinstance(response, dict)
            or response.get("deleted") is not True
        ):
            raise KeyJsonProtocolError("unexpected response to DELETE")

    def keys(self) -> list[str]:
        """Return all stored keys; their order is not guaranteed."""
        _, response = self._request("GET", "/keys")
        if not isinstance(response, list) or not all(
            isinstance(key, str) for key in response
        ):
            raise KeyJsonProtocolError("unexpected response to GET /keys")
        return response

    def stats(self) -> dict[str, JSONValue]:
        """Return the server's operation and storage counters."""
        _, response = self._request("GET", "/stats")
        if not isinstance(response, dict):
            raise KeyJsonProtocolError("unexpected response to GET /stats")
        for field in _STATS_FIELDS:
            value = response.get(field)
            if type(value) is not int or value < 0:
                raise KeyJsonProtocolError("unexpected response to GET /stats")
        return response
