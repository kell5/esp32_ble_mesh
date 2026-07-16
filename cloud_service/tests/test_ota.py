from __future__ import annotations

import json
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from fastapi.testclient import TestClient

from cloud_service.config import Settings
from cloud_service.main import create_app

SHA = "a" * 64


class OtaTest(unittest.TestCase):
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

    def _register_device(self, device_id: str) -> None:
        response = self.client.post(
            "/api/v1/devices",
            headers=self.headers,
            json={"device_id": device_id, "type": "light_bulb"},
        )
        self.assertEqual(response.status_code, 201, response.text)

    def _register_firmware(self, fw_version: str = "1.1.0", **overrides: object) -> dict:
        body = {
            "product_id": "bulb",
            "hw_version": "rev-a",
            "fw_version": fw_version,
            "url": f"https://ota.example.com/{fw_version}.bin",
            "sha256": SHA,
            "sign": "sig-abc",
        }
        body.update(overrides)
        response = self.client.post("/api/v1/firmware", headers=self.headers, json=body)
        self.assertEqual(response.status_code, 201, response.text)
        return response.json()

    def _create_rollout(self, **overrides: object) -> dict:
        body: dict = {
            "product_id": "bulb",
            "hw_version": "rev-a",
            "target_fw_version": "1.1.0",
            "percent": 100,
        }
        body.update(overrides)
        response = self.client.post("/api/v1/rollouts", headers=self.headers, json=body)
        self.assertEqual(response.status_code, 201, response.text)
        return response.json()

    def _check(self, device_id: str, fw_version: str = "1.0.0") -> dict:
        response = self.client.post(
            f"/api/v1/devices/{device_id}/ota/check",
            headers=self.headers,
            json={"product_id": "bulb", "hw_version": "rev-a", "fw_version": fw_version},
        )
        self.assertEqual(response.status_code, 200, response.text)
        return response.json()

    def test_firmware_register_is_upsert_and_listable(self) -> None:
        first = self._register_firmware()
        again = self._register_firmware(url="https://ota.example.com/new.bin")
        self.assertEqual(first["firmware_id"], again["firmware_id"])
        self.assertEqual(again["url"], "https://ota.example.com/new.bin")

        listed = self.client.get(
            "/api/v1/firmware", headers=self.headers, params={"product_id": "bulb"}
        )
        self.assertEqual(listed.status_code, 200, listed.text)
        self.assertEqual([item["fw_version"] for item in listed.json()], ["1.1.0"])

    def test_rejects_invalid_sha256(self) -> None:
        response = self.client.post(
            "/api/v1/firmware",
            headers=self.headers,
            json={
                "product_id": "bulb",
                "hw_version": "rev-a",
                "fw_version": "1.1.0",
                "url": "https://ota.example.com/x.bin",
                "sha256": "not-a-hash",
            },
        )
        self.assertEqual(response.status_code, 422, response.text)

    def test_dispatch_matches_and_carries_url_sha256_sign(self) -> None:
        self._register_device("node-ota")
        firmware = self._register_firmware()
        self._create_rollout()

        result = self._check("node-ota")
        self.assertTrue(result["dispatched"])
        self.assertEqual(result["reason"], "dispatched")
        self.assertFalse(result["published"])  # mqtt disabled in tests
        self.assertEqual(result["target"]["firmware_id"], firmware["firmware_id"])
        self.assertEqual(result["target"]["url"], firmware["url"])
        self.assertEqual(result["target"]["sha256"], SHA)
        self.assertEqual(result["target"]["sign"], "sig-abc")
        self.assertEqual(result["update"]["status"], "pending")
        self.assertEqual(result["update"]["target_fw_version"], "1.1.0")
        self.assertIsNotNone(result["update"]["message_id"])

    def test_no_rollout_does_not_dispatch(self) -> None:
        self._register_device("node-none")
        self._register_firmware()
        result = self._check("node-none")
        self.assertFalse(result["dispatched"])
        self.assertEqual(result["reason"], "no_rollout")
        self.assertIsNone(result["update"])

    def test_missing_firmware_does_not_dispatch(self) -> None:
        self._register_device("node-missing")
        # Rollout points at a firmware version that was never uploaded.
        self._create_rollout(target_fw_version="9.9.9")
        result = self._check("node-missing")
        self.assertFalse(result["dispatched"])
        self.assertEqual(result["reason"], "firmware_missing")

    def test_up_to_date_does_not_dispatch(self) -> None:
        self._register_device("node-current")
        self._register_firmware()
        self._create_rollout()
        result = self._check("node-current", fw_version="1.1.0")
        self.assertFalse(result["dispatched"])
        self.assertEqual(result["reason"], "up_to_date")

    def test_from_fw_version_filters_devices(self) -> None:
        self._register_device("node-a")
        self._register_device("node-b")
        self._register_firmware()
        self._create_rollout(from_fw_version="1.0.5")

        matched = self._check("node-a", fw_version="1.0.5")
        self.assertTrue(matched["dispatched"])
        skipped = self._check("node-b", fw_version="1.0.0")
        self.assertFalse(skipped["dispatched"])
        self.assertEqual(skipped["reason"], "no_rollout")

    def test_dispatch_is_idempotent(self) -> None:
        self._register_device("node-idem")
        self._register_firmware()
        self._create_rollout()

        first = self._check("node-idem")
        second = self._check("node-idem")
        self.assertTrue(first["dispatched"])
        self.assertFalse(second["dispatched"])
        self.assertEqual(second["reason"], "already_dispatched")
        self.assertEqual(second["update"]["update_id"], first["update"]["update_id"])

        updates = self.client.get(
            "/api/v1/devices/node-idem/ota/updates", headers=self.headers
        )
        self.assertEqual(updates.status_code, 200, updates.text)
        self.assertEqual(len(updates.json()), 1)

    def test_zero_percent_never_selects_full_percent_always(self) -> None:
        self._register_device("node-zero")
        self._register_device("node-full")
        self._register_firmware()

        self._create_rollout(percent=0)
        zero = self._check("node-zero")
        self.assertFalse(zero["dispatched"])
        self.assertEqual(zero["reason"], "not_selected")

        # Disable the 0% rule and add a 100% rule (most recent wins).
        self._create_rollout(percent=100)
        full = self._check("node-full")
        self.assertTrue(full["dispatched"])

    def test_gray_percentage_selects_a_deterministic_subset(self) -> None:
        self._register_firmware()
        self._create_rollout(percent=50)
        dispatched = 0
        for index in range(200):
            device_id = f"gray-{index}"
            self._register_device(device_id)
            if self._check(device_id)["dispatched"]:
                dispatched += 1
        self.assertGreater(dispatched, 0)
        self.assertLess(dispatched, 200)

    def test_progress_updates_status(self) -> None:
        self._register_device("node-prog")
        self._register_firmware()
        self._create_rollout()
        self._check("node-prog")

        progress = self.client.post(
            "/api/v1/devices/node-prog/ota/progress",
            headers=self.headers,
            json={"status": "success", "fw_version": "1.1.0"},
        )
        self.assertEqual(progress.status_code, 200, progress.text)
        self.assertEqual(progress.json()["status"], "success")

    def test_progress_without_dispatch_is_404(self) -> None:
        self._register_device("node-empty")
        response = self.client.post(
            "/api/v1/devices/node-empty/ota/progress",
            headers=self.headers,
            json={"status": "failed"},
        )
        self.assertEqual(response.status_code, 404, response.text)

    def test_version_report_over_mqtt_triggers_dispatch(self) -> None:
        self._register_firmware()
        self._create_rollout()
        self.app.state.mqtt_bridge.ingest(
            "farmely/light/node-mqtt/up/status",
            json.dumps(
                {
                    "v": 1,
                    "msg_id": "ver-1",
                    "type": "status",
                    "data": {
                        "online": True,
                        "type": "light_bulb",
                        "product_id": "bulb",
                        "hw_version": "rev-a",
                        "fw_version": "1.0.0",
                    },
                }
            ).encode(),
        )
        updates = self.client.get(
            "/api/v1/devices/node-mqtt/ota/updates", headers=self.headers
        )
        self.assertEqual(updates.status_code, 200, updates.text)
        body = updates.json()
        self.assertEqual(len(body), 1)
        self.assertEqual(body[0]["target_fw_version"], "1.1.0")
        self.assertEqual(body[0]["status"], "pending")

    def test_version_report_without_matching_rollout_does_not_dispatch(self) -> None:
        self.app.state.mqtt_bridge.ingest(
            "farmely/light/node-quiet/up/status",
            json.dumps(
                {
                    "v": 1,
                    "msg_id": "ver-2",
                    "type": "status",
                    "data": {
                        "online": True,
                        "type": "light_bulb",
                        "product_id": "bulb",
                        "hw_version": "rev-a",
                        "fw_version": "1.0.0",
                    },
                }
            ).encode(),
        )
        updates = self.client.get(
            "/api/v1/devices/node-quiet/ota/updates", headers=self.headers
        )
        self.assertEqual(updates.status_code, 200, updates.text)
        self.assertEqual(updates.json(), [])

    def test_firmware_endpoints_require_token(self) -> None:
        response = self.client.get("/api/v1/firmware")
        self.assertEqual(response.status_code, 401, response.text)


if __name__ == "__main__":
    unittest.main()
