from __future__ import annotations

import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from fastapi.testclient import TestClient

from cloud_service.config import Settings
from cloud_service.main import create_app


class AuthApiTest(unittest.TestCase):
    def setUp(self) -> None:
        self._directory = tempfile.TemporaryDirectory()
        settings = replace(
            Settings.from_env(),
            database_path=Path(self._directory.name) / "test.db",
            api_token="admin-token",
            mqtt_enabled=False,
        )
        self.app = create_app(settings)
        self.client = TestClient(self.app)
        self.client.__enter__()
        self.admin = {"X-Cloud-Token": "admin-token"}

    def tearDown(self) -> None:
        self.client.__exit__(None, None, None)
        self._directory.cleanup()

    def _register_account(self, email: str, password: str = "secret123") -> dict:
        response = self.client.post(
            "/api/v1/auth/register", json={"email": email, "password": password}
        )
        self.assertEqual(response.status_code, 201, response.text)
        return response.json()

    def _bearer(self, token: str) -> dict:
        return {"Authorization": f"Bearer {token}"}

    def _provision_device(self, device_id: str) -> None:
        created = self.client.post(
            "/api/v1/devices",
            headers=self.admin,
            json={"device_id": device_id, "type": "light_bulb"},
        )
        self.assertEqual(created.status_code, 201, created.text)

    def test_register_login_and_me(self) -> None:
        account = self._register_account("Alice@Example.com")
        self.assertTrue(account["user_id"].startswith("user-"))
        self.assertEqual(account["email"], "alice@example.com")

        login = self.client.post(
            "/api/v1/auth/login",
            json={"email": "alice@example.com", "password": "secret123"},
        )
        self.assertEqual(login.status_code, 200, login.text)
        token = login.json()["token"]

        me = self.client.get("/api/v1/auth/me", headers=self._bearer(token))
        self.assertEqual(me.status_code, 200, me.text)
        self.assertEqual(me.json()["email"], "alice@example.com")

    def test_duplicate_email_rejected(self) -> None:
        self._register_account("dup@example.com")
        again = self.client.post(
            "/api/v1/auth/register",
            json={"email": "dup@example.com", "password": "another1"},
        )
        self.assertEqual(again.status_code, 409, again.text)

    def test_wrong_password_rejected(self) -> None:
        self._register_account("bob@example.com")
        login = self.client.post(
            "/api/v1/auth/login",
            json={"email": "bob@example.com", "password": "wrongpass"},
        )
        self.assertEqual(login.status_code, 401, login.text)

    def test_claim_and_list_own_devices(self) -> None:
        token = self._register_account("carol@example.com")["token"]
        self._provision_device("node-100")

        claim = self.client.post(
            "/api/v1/me/devices/node-100/claim", headers=self._bearer(token)
        )
        self.assertEqual(claim.status_code, 200, claim.text)
        self.assertEqual(claim.json()["owner_id"], self._me_id(token))

        listing = self.client.get("/api/v1/me/devices", headers=self._bearer(token))
        self.assertEqual(listing.status_code, 200, listing.text)
        self.assertEqual([d["device_id"] for d in listing.json()], ["node-100"])

    def test_account_isolation_blocks_foreign_device(self) -> None:
        owner_token = self._register_account("owner@example.com")["token"]
        other_token = self._register_account("other@example.com")["token"]
        self._provision_device("node-200")
        self.client.post(
            "/api/v1/me/devices/node-200/claim", headers=self._bearer(owner_token)
        )

        # Owner can read the shadow.
        ok = self.client.get(
            "/api/v1/devices/node-200/shadow", headers=self._bearer(owner_token)
        )
        self.assertEqual(ok.status_code, 200, ok.text)

        # A different account is forbidden.
        forbidden = self.client.get(
            "/api/v1/devices/node-200/shadow", headers=self._bearer(other_token)
        )
        self.assertEqual(forbidden.status_code, 403, forbidden.text)

        # The other account does not see it in their list.
        listing = self.client.get("/api/v1/me/devices", headers=self._bearer(other_token))
        self.assertEqual(listing.json(), [])

    def test_double_claim_conflict(self) -> None:
        first = self._register_account("first@example.com")["token"]
        second = self._register_account("second@example.com")["token"]
        self._provision_device("node-300")
        self.client.post("/api/v1/me/devices/node-300/claim", headers=self._bearer(first))
        conflict = self.client.post(
            "/api/v1/me/devices/node-300/claim", headers=self._bearer(second)
        )
        self.assertEqual(conflict.status_code, 409, conflict.text)

    def test_unclaim_removes_device_and_allows_reclaim(self) -> None:
        token = self._register_account("erin@example.com")["token"]
        self._provision_device("node-500")
        self.client.post("/api/v1/me/devices/node-500/claim", headers=self._bearer(token))

        removed = self.client.delete(
            "/api/v1/me/devices/node-500", headers=self._bearer(token)
        )
        self.assertEqual(removed.status_code, 204, removed.text)

        listing = self.client.get("/api/v1/me/devices", headers=self._bearer(token))
        self.assertEqual(listing.json(), [])

        # The device is unclaimed, so it can be claimed again (by anyone).
        reclaim = self.client.post(
            "/api/v1/me/devices/node-500/claim", headers=self._bearer(token)
        )
        self.assertEqual(reclaim.status_code, 200, reclaim.text)

    def test_unclaim_foreign_device_is_forbidden(self) -> None:
        owner = self._register_account("frank@example.com")["token"]
        other = self._register_account("grace@example.com")["token"]
        self._provision_device("node-600")
        self.client.post("/api/v1/me/devices/node-600/claim", headers=self._bearer(owner))

        forbidden = self.client.delete(
            "/api/v1/me/devices/node-600", headers=self._bearer(other)
        )
        self.assertEqual(forbidden.status_code, 403, forbidden.text)

    def test_admin_token_still_has_access(self) -> None:
        self._provision_device("node-400")
        shadow = self.client.get(
            "/api/v1/devices/node-400/shadow", headers=self.admin
        )
        self.assertEqual(shadow.status_code, 200, shadow.text)

    def test_invalid_bearer_rejected(self) -> None:
        me = self.client.get("/api/v1/auth/me", headers=self._bearer("nope"))
        self.assertEqual(me.status_code, 401, me.text)

    def test_logout_revokes_token(self) -> None:
        token = self._register_account("dave@example.com")["token"]
        logout = self.client.post("/api/v1/auth/logout", headers=self._bearer(token))
        self.assertEqual(logout.status_code, 204, logout.text)
        me = self.client.get("/api/v1/auth/me", headers=self._bearer(token))
        self.assertEqual(me.status_code, 401, me.text)

    def _me_id(self, token: str) -> str:
        return self.client.get("/api/v1/auth/me", headers=self._bearer(token)).json()[
            "user_id"
        ]


if __name__ == "__main__":
    unittest.main()
