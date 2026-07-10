from __future__ import annotations

import json
import tempfile
import unittest
from dataclasses import replace
from datetime import datetime, timedelta, timezone
from pathlib import Path

from fastapi.testclient import TestClient

from cloud_service.config import Settings
from cloud_service.main import create_app


class CloudApiTest(unittest.TestCase):
    def setUp(self) -> None:
        self._directory = tempfile.TemporaryDirectory()
        settings = replace(
            Settings.from_env(),
            database_path=Path(self._directory.name) / "test.db",
            api_token="test-token",
            mqtt_enabled=False,
        )
        self.app = create_app(settings)
        self.client = TestClient(self.app)
        self.client.__enter__()
        self.headers = {"X-Cloud-Token": "test-token"}

    def tearDown(self) -> None:
        self.client.__exit__(None, None, None)
        self._directory.cleanup()

    def _register(self, device_id: str = "node-001") -> None:
        response = self.client.post(
            "/api/v1/devices",
            headers=self.headers,
            json={
                "device_id": device_id,
                "type": "light_bulb",
                "name": "书房灯",
                "metadata": {"source": "test"},
            },
        )
        self.assertEqual(response.status_code, 201, response.text)

    def test_register_claim_and_list(self) -> None:
        self._register()
        claim = self.client.post(
            "/api/v1/devices/node-001/claim",
            headers=self.headers,
            json={"user_id": "user-001"},
        )
        self.assertEqual(claim.status_code, 200, claim.text)
        self.assertEqual(claim.json()["owner_id"], "user-001")

        devices = self.client.get("/api/v1/users/user-001/devices", headers=self.headers)
        self.assertEqual(devices.status_code, 200, devices.text)
        self.assertEqual([item["device_id"] for item in devices.json()], ["node-001"])

    def test_claim_conflict(self) -> None:
        self._register()
        first = self.client.post(
            "/api/v1/devices/node-001/claim",
            headers=self.headers,
            json={"user_id": "user-001"},
        )
        self.assertEqual(first.status_code, 200, first.text)
        second = self.client.post(
            "/api/v1/devices/node-001/claim",
            headers=self.headers,
            json={"user_id": "user-002"},
        )
        self.assertEqual(second.status_code, 409, second.text)

    def test_desired_state_is_idempotent(self) -> None:
        self._register()
        body = {"state": {"on": True}, "message_id": "command-001"}
        first = self.client.patch(
            "/api/v1/devices/node-001/shadow/desired",
            headers=self.headers,
            json=body,
        )
        second = self.client.patch(
            "/api/v1/devices/node-001/shadow/desired",
            headers=self.headers,
            json=body,
        )
        self.assertEqual(first.status_code, 200, first.text)
        self.assertTrue(first.json()["changed"])
        self.assertEqual(first.json()["shadow"]["desired_version"], 1)
        self.assertFalse(second.json()["changed"])
        self.assertEqual(second.json()["shadow"]["desired_version"], 1)

    def test_mqtt_status_updates_reported_shadow(self) -> None:
        payload = json.dumps(
            {
                "id": "node-002",
                "online": True,
                "state": "on",
                "layer": 2,
                "role": "node",
                "type": "socket",
            }
        ).encode()
        self.app.state.mqtt_bridge.ingest("office/light/node/node-002/status", payload)
        shadow = self.client.get("/api/v1/devices/node-002/shadow", headers=self.headers)
        self.assertEqual(shadow.status_code, 200, shadow.text)
        reported = shadow.json()["reported"]
        self.assertTrue(reported["online"])
        self.assertTrue(reported["on"])
        self.assertEqual(reported["type"], "socket")

    def test_stale_device_gets_offline_reason(self) -> None:
        payload = json.dumps({"online": True, "state": "off", "type": "relay"}).encode()
        self.app.state.mqtt_bridge.ingest("office/light/node/node-stale/status", payload)
        cutoff = datetime.now(timezone.utc) + timedelta(seconds=1)
        marked = self.app.state.store.mark_stale_devices(cutoff)
        self.assertEqual(marked, ["node-stale"])
        shadow = self.client.get("/api/v1/devices/node-stale/shadow", headers=self.headers).json()
        self.assertFalse(shadow["reported"]["online"])
        self.assertEqual(shadow["offline_reason"], "cloud_timeout")

    def test_plain_doorbell_event_updates_shadow(self) -> None:
        self.app.state.mqtt_bridge.ingest("doorbell/door-001/event", b"ringing")
        shadow = self.client.get("/api/v1/devices/door-001/shadow", headers=self.headers).json()
        self.assertTrue(shadow["reported"]["online"])
        self.assertEqual(shadow["reported"]["last_event"], "ringing")

    def test_mutation_requires_token_when_configured(self) -> None:
        response = self.client.post(
            "/api/v1/devices",
            json={"device_id": "node-003", "type": "relay"},
        )
        self.assertEqual(response.status_code, 401, response.text)


if __name__ == "__main__":
    unittest.main()
