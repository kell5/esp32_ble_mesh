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

    def test_capabilities_are_returned_for_current_user_devices_and_shadow(self) -> None:
        account = self.client.post(
            "/api/v1/auth/register",
            json={"email": "caps@example.com", "password": "secret123"},
        )
        self.assertEqual(account.status_code, 201, account.text)
        bearer = {"Authorization": f"Bearer {account.json()['token']}"}
        created = self.client.post(
            "/api/v1/devices",
            headers=self.headers,
            json={
                "device_id": "node-cap",
                "type": "light_bulb",
                "capabilities": ["onoff", "sensor.temperature"],
            },
        )
        self.assertEqual(created.status_code, 201, created.text)
        self.assertEqual(created.json()["capabilities"], ["onoff", "sensor.temperature"])

        claim = self.client.post("/api/v1/me/devices/node-cap/claim", headers=bearer)
        self.assertEqual(claim.status_code, 200, claim.text)
        devices = self.client.get("/api/v1/me/devices", headers=bearer)
        self.assertEqual(devices.status_code, 200, devices.text)
        self.assertEqual(devices.json()[0]["capabilities"], ["onoff", "sensor.temperature"])

        shadow = self.client.get("/api/v1/devices/node-cap/shadow", headers=bearer)
        self.assertEqual(shadow.status_code, 200, shadow.text)
        self.assertEqual(shadow.json()["capabilities"], ["onoff", "sensor.temperature"])

    def test_legacy_device_without_capabilities_remains_readable(self) -> None:
        self._register("node-legacy")
        device = self.client.get("/api/v1/devices/node-legacy", headers=self.headers)
        self.assertEqual(device.status_code, 200, device.text)
        self.assertEqual(device.json()["capabilities"], [])

        shadow = self.client.get("/api/v1/devices/node-legacy/shadow", headers=self.headers)
        self.assertEqual(shadow.status_code, 200, shadow.text)
        self.assertEqual(shadow.json()["capabilities"], [])

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

    def test_normalized_and_legacy_light_topics_update_reported_shadow(self) -> None:
        bridge = self.app.state.mqtt_bridge
        bridge.ingest(
            "farmely/light/node-new/up/status",
            json.dumps(
                {
                    "v": 1,
                    "msg_id": "new-status-1",
                    "ts": 1730000000,
                    "type": "status",
                    "data": {
                        "online": True,
                        "state": "on",
                        "type": "socket",
                        "capabilities": ["onoff"],
                    },
                }
            ).encode(),
        )
        bridge.ingest(
            "farmely/light/node-new/up/status",
            json.dumps(
                {
                    "v": 1,
                    "msg_id": "new-status-1",
                    "ts": 1730000001,
                    "type": "status",
                    "data": {"online": True, "state": "off", "type": "socket"},
                }
            ).encode(),
        )
        new_shadow = self.client.get(
            "/api/v1/devices/node-new/shadow", headers=self.headers
        ).json()
        self.assertTrue(new_shadow["reported"]["on"])
        self.assertEqual(new_shadow["capabilities"], ["onoff"])
        self.assertEqual(new_shadow["reported_version"], 1)

        bridge.ingest(
            "office/light/node/node-old/status",
            json.dumps({"online": True, "state": "off", "type": "relay"}).encode(),
        )
        old_shadow = self.client.get(
            "/api/v1/devices/node-old/shadow", headers=self.headers
        ).json()
        self.assertFalse(old_shadow["reported"]["on"])

    def test_normalized_doorbell_event_updates_shadow_and_dedupes(self) -> None:
        bridge = self.app.state.mqtt_bridge
        envelope = {
            "v": 1,
            "msg_id": "door-event-1",
            "ts": 1730000000,
            "type": "event",
            "data": {
                "event": "ringing",
                "type": "doorbell",
                "capabilities": ["doorbell.ring"],
            },
        }
        bridge.ingest("farmely/doorbell/door-new/up/event", json.dumps(envelope).encode())
        envelope["data"]["event"] = "ignored"
        bridge.ingest("farmely/doorbell/door-new/up/event", json.dumps(envelope).encode())

        shadow = self.client.get("/api/v1/devices/door-new/shadow", headers=self.headers)
        self.assertEqual(shadow.status_code, 200, shadow.text)
        self.assertEqual(shadow.json()["reported"]["last_event"], "ringing")
        self.assertEqual(shadow.json()["capabilities"], ["doorbell.ring"])

        events = self.client.get("/api/v1/devices/door-new/events", headers=self.headers)
        self.assertEqual(events.status_code, 200, events.text)
        self.assertEqual([item["event"] for item in events.json()], ["ringing"])

    def test_normalized_ack_on_event_channel_is_not_recorded_as_event(self) -> None:
        envelope = {
            "v": 1,
            "msg_id": "door-ack-1",
            "ts": 1730000000,
            "type": "ack",
            "data": {"ack_msg_id": "cloud-command-1"},
        }
        self.app.state.mqtt_bridge.ingest(
            "farmely/doorbell/door-new/up/event",
            json.dumps(envelope).encode(),
        )
        self.app.state.mqtt_bridge.ingest(
            "doorbell/door-new/event",
            json.dumps(
                {
                    "version": 1,
                    "message_id": "door-ack-legacy-1",
                    "event": "ack",
                    "ack_msg_id": "cloud-command-1",
                }
            ).encode(),
        )

        shadow = self.client.get("/api/v1/devices/door-new/shadow", headers=self.headers)
        self.assertEqual(shadow.status_code, 200, shadow.text)
        self.assertEqual(
            shadow.json()["reported"]["ack_msg_id"],
            "cloud-command-1",
        )

        events = self.client.get("/api/v1/devices/door-new/events", headers=self.headers)
        self.assertEqual(events.status_code, 200, events.text)
        self.assertEqual(events.json(), [])

    def test_stale_device_gets_offline_reason(self) -> None:
        payload = json.dumps({"online": True, "state": "off", "type": "relay"}).encode()
        self.app.state.mqtt_bridge.ingest("office/light/node/node-stale/status", payload)
        cutoff = datetime.now(timezone.utc) + timedelta(seconds=1)
        marked = self.app.state.store.mark_stale_devices(cutoff)
        self.assertEqual(marked, ["node-stale"])
        shadow = self.client.get("/api/v1/devices/node-stale/shadow", headers=self.headers).json()
        self.assertFalse(shadow["reported"]["online"])
        self.assertEqual(shadow["offline_reason"], "cloud_timeout")

    def test_doorbell_lwt_updates_offline_shadow(self) -> None:
        payload = json.dumps(
            {
                "version": 1,
                "message_id": "door-001-boot-1",
                "online": False,
                "type": "doorbell",
                "offline_reason": "mqtt_lwt",
            }
        ).encode()
        self.app.state.mqtt_bridge.ingest("doorbell/door-001/status", payload)
        shadow = self.client.get("/api/v1/devices/door-001/shadow", headers=self.headers).json()
        self.assertFalse(shadow["reported"]["online"])
        self.assertEqual(shadow["reported"]["version"], 1)
        self.assertEqual(shadow["offline_reason"], "mqtt_lwt")

    def test_plain_doorbell_event_updates_shadow(self) -> None:
        self.app.state.mqtt_bridge.ingest("doorbell/door-001/event", b"ringing")
        shadow = self.client.get("/api/v1/devices/door-001/shadow", headers=self.headers).json()
        self.assertTrue(shadow["reported"]["online"])
        self.assertEqual(shadow["reported"]["last_event"], "ringing")

    def test_doorbell_events_are_recorded_and_listable(self) -> None:
        bridge = self.app.state.mqtt_bridge
        bridge.ingest(
            "doorbell/door-001/event",
            json.dumps({"version": 1, "message_id": "evt-1", "event": "ringing"}).encode(),
        )
        bridge.ingest(
            "doorbell/door-001/event",
            json.dumps({"version": 1, "message_id": "evt-2", "event": "stream_stop"}).encode(),
        )
        # Duplicate message_id must be ignored (idempotent).
        bridge.ingest(
            "doorbell/door-001/event",
            json.dumps({"version": 1, "message_id": "evt-1", "event": "ringing"}).encode(),
        )

        events = self.client.get(
            "/api/v1/devices/door-001/events", headers=self.headers
        )
        self.assertEqual(events.status_code, 200, events.text)
        body = events.json()
        self.assertEqual([item["event"] for item in body], ["stream_stop", "ringing"])
        self.assertEqual(body[0]["payload"]["event"], "stream_stop")

    def test_mutation_requires_token_when_configured(self) -> None:
        response = self.client.post(
            "/api/v1/devices",
            json={"device_id": "node-003", "type": "relay"},
        )
        self.assertEqual(response.status_code, 401, response.text)


if __name__ == "__main__":
    unittest.main()
