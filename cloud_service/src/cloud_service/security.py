from __future__ import annotations

import hashlib
import hmac
import secrets
from dataclasses import dataclass

_PBKDF2_ITERATIONS = 120_000
_ALGORITHM = "pbkdf2_sha256"


def hash_password(password: str) -> str:
    salt = secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac(
        "sha256", password.encode("utf-8"), salt, _PBKDF2_ITERATIONS
    )
    return f"{_ALGORITHM}${_PBKDF2_ITERATIONS}${salt.hex()}${digest.hex()}"


def verify_password(password: str, encoded: str) -> bool:
    try:
        algorithm, iterations_text, salt_hex, digest_hex = encoded.split("$")
    except ValueError:
        return False
    if algorithm != _ALGORITHM:
        return False
    try:
        iterations = int(iterations_text)
        salt = bytes.fromhex(salt_hex)
        expected = bytes.fromhex(digest_hex)
    except ValueError:
        return False
    candidate = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, iterations)
    return hmac.compare_digest(candidate, expected)


def new_token() -> str:
    return secrets.token_urlsafe(32)


@dataclass(frozen=True)
class Principal:
    """Result of authenticating a request.

    ``is_admin`` marks the shared ``X-Cloud-Token`` (device provisioning / admin).
    ``user_id`` is set when a per-user bearer token authenticated the request.
    """

    is_admin: bool
    user_id: str | None = None
