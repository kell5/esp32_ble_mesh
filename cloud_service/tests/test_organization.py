from __future__ import annotations

import json
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from fastapi.testclient import TestClient

from cloud_service.config import Settings
from cloud_service.main import create_app


class OrganizationApiTest(unittest.TestCase):
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

    def _register(self, device_id: str, device_type: str = "light_bulb") -> None:
        response = self.client.post(
            "/api/v1/devices",
            headers=self.headers,
            json={"device_id": device_id, "type": device_type},
        )
        self.assertEqual(response.status_code, 201, response.text)

    def test_room_lifecycle_and_device_assignment(self) -> None:
        self._register("node-001")
        created = self.client.post(
            "/api/v1/users/user-001/rooms",
            headers=self.headers,
            json={"name": "客厅"},
        )
        self.assertEqual(created.status_code, 201, created.text)
        room_id = created.json()["room_id"]

        assigned = self.client.put(
            f"/api/v1/rooms/{room_id}/devices/node-001", headers=self.headers
        )
        self.assertEqual(assigned.status_code, 200, assigned.text)
        self.assertEqual(assigned.json()["device_ids"], ["node-001"])

        device = self.client.get("/api/v1/devices/node-001", headers=self.headers).json()
        self.assertEqual(device["room_id"], room_id)

        rooms = self.client.get("/api/v1/users/user-001/rooms", headers=self.headers)
        self.assertEqual([item["room_id"] for item in rooms.json()], [room_id])

        removed = self.client.delete(
            f"/api/v1/rooms/{room_id}/devices/node-001", headers=self.headers
        )
        self.assertEqual(removed.json()["device_ids"], [])

        deleted = self.client.delete(f"/api/v1/rooms/{room_id}", headers=self.headers)
        self.assertEqual(deleted.status_code, 204, deleted.text)
        self.assertEqual(
            self.client.get(f"/api/v1/rooms/{room_id}", headers=self.headers).status_code, 404
        )

    def test_deleting_room_clears_device_assignment(self) -> None:
        self._register("node-001")
        room_id = self.client.post(
            "/api/v1/users/user-001/rooms", headers=self.headers, json={"name": "卧室"}
        ).json()["room_id"]
        self.client.put(f"/api/v1/rooms/{room_id}/devices/node-001", headers=self.headers)
        self.client.delete(f"/api/v1/rooms/{room_id}", headers=self.headers)
        device = self.client.get("/api/v1/devices/node-001", headers=self.headers).json()
        self.assertIsNone(device["room_id"])

    def test_group_command_applies_desired_to_members(self) -> None:
        self._register("node-001")
        self._register("node-002")
        group_id = self.client.post(
            "/api/v1/users/user-001/groups", headers=self.headers, json={"name": "全屋灯"}
        ).json()["group_id"]
        self.client.put(f"/api/v1/groups/{group_id}/devices/node-001", headers=self.headers)
        self.client.put(f"/api/v1/groups/{group_id}/devices/node-002", headers=self.headers)

        response = self.client.post(
            f"/api/v1/groups/{group_id}/command",
            headers=self.headers,
            json={"state": {"on": True}},
        )
        self.assertEqual(response.status_code, 200, response.text)
        results = {item["device_id"]: item for item in response.json()["results"]}
        self.assertTrue(results["node-001"]["changed"])
        self.assertTrue(results["node-002"]["changed"])
        self.assertFalse(results["node-001"]["published"])  # MQTT disabled in tests

        shadow = self.client.get(
            "/api/v1/devices/node-001/shadow", headers=self.headers
        ).json()
        self.assertTrue(shadow["desired"]["on"])

    def test_scene_activation_applies_stored_actions(self) -> None:
        self._register("node-001")
        self._register("node-002")
        scene = self.client.post(
            "/api/v1/users/user-001/scenes",
            headers=self.headers,
            json={
                "name": "回家",
                "actions": [
                    {"device_id": "node-001", "state": {"on": True}},
                    {"device_id": "node-002", "state": {"on": False}},
                ],
            },
        )
        self.assertEqual(scene.status_code, 201, scene.text)
        scene_id = scene.json()["scene_id"]

        activation = self.client.post(
            f"/api/v1/scenes/{scene_id}/activate", headers=self.headers
        )
        self.assertEqual(activation.status_code, 200, activation.text)
        self.assertEqual(len(activation.json()["results"]), 2)

        first = self.client.get(
            "/api/v1/devices/node-001/shadow", headers=self.headers
        ).json()
        second = self.client.get(
            "/api/v1/devices/node-002/shadow", headers=self.headers
        ).json()
        self.assertTrue(first["desired"]["on"])
        self.assertFalse(second["desired"]["on"])

    def test_scene_rejects_unknown_device(self) -> None:
        response = self.client.post(
            "/api/v1/users/user-001/scenes",
            headers=self.headers,
            json={"name": "坏场景", "actions": [{"device_id": "ghost", "state": {"on": True}}]},
        )
        self.assertEqual(response.status_code, 404, response.text)

    def test_automation_triggers_scene_on_reported_event(self) -> None:
        self._register("door-001", "doorbell")
        self._register("node-001")
        scene_id = self.client.post(
            "/api/v1/users/user-001/scenes",
            headers=self.headers,
            json={"name": "门铃亮灯", "actions": [{"device_id": "node-001", "state": {"on": True}}]},
        ).json()["scene_id"]

        automation = self.client.post(
            "/api/v1/users/user-001/automations",
            headers=self.headers,
            json={
                "name": "有人按门铃就开灯",
                "trigger": {"device_id": "door-001", "field": "last_event", "equals": "ringing"},
                "action": {"type": "scene", "scene_id": scene_id},
            },
        )
        self.assertEqual(automation.status_code, 201, automation.text)

        self.app.state.mqtt_bridge.ingest("doorbell/door-001/event", b"ringing")

        shadow = self.client.get(
            "/api/v1/devices/node-001/shadow", headers=self.headers
        ).json()
        self.assertTrue(shadow["desired"]["on"])

    def test_disabled_automation_does_not_fire(self) -> None:
        self._register("node-002")
        self._register("node-001")
        automation_id = self.client.post(
            "/api/v1/users/user-001/automations",
            headers=self.headers,
            json={
                "name": "关闭的规则",
                "enabled": False,
                "trigger": {"device_id": "node-002", "field": "on", "equals": True},
                "action": {"type": "device", "device_id": "node-001", "state": {"on": True}},
            },
        ).json()["automation_id"]

        payload = json.dumps({"online": True, "state": "on", "type": "light_bulb"}).encode()
        self.app.state.mqtt_bridge.ingest("office/light/node/node-002/status", payload)
        shadow = self.client.get(
            "/api/v1/devices/node-001/shadow", headers=self.headers
        ).json()
        self.assertEqual(shadow["desired"], {})

        enable = self.client.patch(
            f"/api/v1/automations/{automation_id}",
            headers=self.headers,
            json={"enabled": True},
        )
        self.assertEqual(enable.status_code, 200, enable.text)
        self.app.state.mqtt_bridge.ingest("office/light/node/node-002/status", payload)
        shadow = self.client.get(
            "/api/v1/devices/node-001/shadow", headers=self.headers
        ).json()
        self.assertTrue(shadow["desired"]["on"])

    def test_automation_group_action(self) -> None:
        self._register("sensor-001", "motion_sensor")
        self._register("node-001")
        self._register("node-002")
        group_id = self.client.post(
            "/api/v1/users/user-001/groups", headers=self.headers, json={"name": "走廊灯"}
        ).json()["group_id"]
        self.client.put(f"/api/v1/groups/{group_id}/devices/node-001", headers=self.headers)
        self.client.put(f"/api/v1/groups/{group_id}/devices/node-002", headers=self.headers)

        self.client.post(
            "/api/v1/users/user-001/automations",
            headers=self.headers,
            json={
                "name": "有人经过开走廊灯",
                "trigger": {"device_id": "sensor-001", "field": "on", "equals": True},
                "action": {"type": "group", "group_id": group_id, "state": {"on": True}},
            },
        )
        payload = json.dumps({"online": True, "on": True, "type": "motion_sensor"}).encode()
        self.app.state.mqtt_bridge.ingest("office/light/node/sensor-001/status", payload)
        for device_id in ("node-001", "node-002"):
            shadow = self.client.get(
                f"/api/v1/devices/{device_id}/shadow", headers=self.headers
            ).json()
            self.assertTrue(shadow["desired"]["on"])

    def test_invalid_automation_action_is_rejected(self) -> None:
        self._register("door-001", "doorbell")
        response = self.client.post(
            "/api/v1/users/user-001/automations",
            headers=self.headers,
            json={
                "name": "缺少目标",
                "trigger": {"device_id": "door-001", "field": "last_event", "equals": "ringing"},
                "action": {"type": "scene"},
            },
        )
        self.assertEqual(response.status_code, 422, response.text)

    def test_organization_requires_token(self) -> None:
        response = self.client.post("/api/v1/users/user-001/rooms", json={"name": "无权限"})
        self.assertEqual(response.status_code, 401, response.text)


if __name__ == "__main__":
    unittest.main()
