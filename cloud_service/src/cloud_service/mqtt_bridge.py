from __future__ import annotations

import json
import time
import uuid
from threading import Event
from typing import Callable, cast

from paho.mqtt import client as mqtt
from pydantic import JsonValue

from .config import Settings
from .models import FirmwareResponse
from .storage import DeviceStore

FARMELY_ROOT = "farmely"
FARMELY_UP_FILTER = f"{FARMELY_ROOT}/+/+/up/+"


def _topic_value(pattern: str, topic: str) -> str | None:
    if "+" not in pattern:
        return None
    prefix, suffix = pattern.split("+", 1)
    if not topic.startswith(prefix) or not topic.endswith(suffix):
        return None
    value = topic[len(prefix) : len(topic) - len(suffix) if suffix else None]
    return value or None


def _scalar(value: object) -> JsonValue | None:
    if value is None or isinstance(value, str | int | float | bool):
        return value
    return None


def _json_object(value: object) -> dict[str, JsonValue] | None:
    if not isinstance(value, dict):
        return None
    result: dict[str, JsonValue] = {}
    for key, item in value.items():
        if isinstance(key, str):
            result[key] = cast(JsonValue, item)
    return result


def _string_list(value: object) -> list[str] | None:
    if not isinstance(value, list):
        return None
    return [item for item in value if isinstance(item, str)]


def _message_id(document: dict[str, JsonValue]) -> str | None:
    value = document.get("msg_id", document.get("message_id"))
    return value if isinstance(value, str) else None


class MqttBridge:
    def __init__(
        self,
        store: DeviceStore,
        settings: Settings,
        on_reported: Callable[[str], None] | None = None,
    ) -> None:
        self._store = store
        self._settings = settings
        self._on_reported = on_reported
        self._on_version_report: Callable[[str, str, str, str], None] | None = None
        self._client: mqtt.Client | None = None
        self._connected = Event()

    @property
    def connected(self) -> bool:
        return self._connected.is_set()

    def set_reported_handler(self, handler: Callable[[str], None]) -> None:
        self._on_reported = handler

    def set_version_handler(self, handler: Callable[[str, str, str, str], None]) -> None:
        self._on_version_report = handler

    def start(self) -> None:
        if self._client is not None:
            return
        client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=self._settings.mqtt_client_id,
            protocol=mqtt.MQTTv311,
        )
        if self._settings.mqtt_username is not None:
            client.username_pw_set(self._settings.mqtt_username, self._settings.mqtt_password)
        client.on_connect = self._on_connect
        client.on_disconnect = self._on_disconnect
        client.on_message = self._on_message
        client.reconnect_delay_set(min_delay=1, max_delay=30)
        client.connect_async(self._settings.mqtt_host, self._settings.mqtt_port, keepalive=30)
        client.loop_start()
        self._client = client

    def stop(self) -> None:
        client = self._client
        self._client = None
        self._connected.clear()
        if client is None:
            return
        client.disconnect()
        client.loop_stop()

    def publish_desired(self, device_id: str, state: dict[str, JsonValue]) -> bool:
        client = self._client
        if client is None or not self.connected:
            return False

        on = state.get("on")
        if isinstance(on, bool):
            device_class = "light"
            legacy_topic = f"office/light/node/{device_id}/cmd"
            legacy_payload = "on" if on else "off"
        else:
            command = state.get("command")
            if not isinstance(command, str):
                return False
            device_class = "doorbell"
            legacy_topic = f"doorbell/{device_id}/cmd"
            legacy_payload = command

        envelope = {
            "v": 1,
            "msg_id": f"cloud-{uuid.uuid4().hex}",
            "ts": int(time.time()),
            "type": "cmd",
            "data": state,
        }
        normalized = client.publish(
            f"{FARMELY_ROOT}/{device_class}/{device_id}/down/cmd",
            json.dumps(envelope, ensure_ascii=False, separators=(",", ":")),
            qos=1,
            retain=False,
        )
        legacy = client.publish(legacy_topic, legacy_payload, qos=1, retain=False)
        return (
            normalized.rc == mqtt.MQTT_ERR_SUCCESS
            or legacy.rc == mqtt.MQTT_ERR_SUCCESS
        )

    def publish_ota(
        self,
        device_id: str,
        device_class: str,
        firmware: FirmwareResponse,
        message_id: str,
    ) -> bool:
        client = self._client
        if client is None or not self.connected:
            return False
        envelope = {
            "v": 1,
            "msg_id": message_id,
            "ts": int(time.time()),
            "type": "ota",
            "data": {
                "command": "update",
                "product_id": firmware.product_id,
                "hw_version": firmware.hw_version,
                "fw_version": firmware.fw_version,
                "url": firmware.url,
                "sha256": firmware.sha256,
                "sign": firmware.sign,
            },
        }
        result = client.publish(
            f"{FARMELY_ROOT}/{device_class}/{device_id}/down/ota",
            json.dumps(envelope, ensure_ascii=False, separators=(",", ":")),
            qos=1,
            retain=False,
        )
        return result.rc == mqtt.MQTT_ERR_SUCCESS

    def _apply_reported(
        self,
        device_id: str,
        reported: dict[str, JsonValue],
        message_id: str | None,
        offline_reason: str | None,
    ) -> None:
        _shadow, changed = self._store.update_reported(
            device_id,
            reported,
            message_id=message_id,
            offline_reason=offline_reason,
        )
        if changed and self._on_reported is not None:
            self._on_reported(device_id)
        if changed:
            self._maybe_trigger_ota(device_id, reported)

    def _maybe_trigger_ota(self, device_id: str, reported: dict[str, JsonValue]) -> None:
        handler = self._on_version_report
        if handler is None:
            return
        product_id = reported.get("product_id")
        hw_version = reported.get("hw_version")
        fw_version = reported.get("fw_version", reported.get("version"))
        if not isinstance(product_id, str) or not isinstance(hw_version, str):
            return
        if isinstance(fw_version, int | float) and not isinstance(fw_version, bool):
            fw_version = str(fw_version)
        if not isinstance(fw_version, str) or not fw_version:
            return
        handler(device_id, product_id, hw_version, fw_version)

    def ingest(self, topic: str, payload: bytes) -> None:
        text = payload.decode("utf-8", errors="replace")
        try:
            document = json.loads(text)
        except json.JSONDecodeError:
            document = text

        if self._ingest_farmely(topic, document):
            return

        node_id = _topic_value(self._settings.mqtt_node_status_filter, topic)
        if node_id is not None and isinstance(document, dict):
            self._ingest_node(node_id, document)
            return

        if topic == self._settings.mqtt_gateway_status_topic and isinstance(document, dict):
            self._ingest_gateway(document)
            return

        doorbell_id = _topic_value(self._settings.mqtt_doorbell_status_filter, topic)
        if doorbell_id is not None and isinstance(document, dict):
            self._ingest_doorbell_status(doorbell_id, document)
            return

        doorbell_id = _topic_value(self._settings.mqtt_doorbell_event_filter, topic)
        if doorbell_id is not None:
            self._ingest_doorbell(doorbell_id, document)

    def _on_connect(self, client, _userdata, _flags, reason_code, _properties) -> None:
        if reason_code != 0:
            self._connected.clear()
            return
        self._connected.set()
        client.subscribe(
            [
                (FARMELY_UP_FILTER, 1),
                (self._settings.mqtt_node_status_filter, 1),
                (self._settings.mqtt_gateway_status_topic, 1),
                (self._settings.mqtt_doorbell_status_filter, 1),
                (self._settings.mqtt_doorbell_event_filter, 1),
            ]
        )

    def _on_disconnect(self, _client, _userdata, _flags, _reason_code, _properties) -> None:
        self._connected.clear()

    def _on_message(self, _client, _userdata, message: mqtt.MQTTMessage) -> None:
        self.ingest(message.topic, message.payload)

    def _ingest_farmely(self, topic: str, document: object) -> bool:
        parts = topic.split("/")
        if len(parts) != 5:
            return False
        root, device_class, device_id, direction, channel = parts
        if root != FARMELY_ROOT or direction != "up":
            return False
        envelope_type, message_id, data = self._unwrap_envelope(document)
        if data is None:
            return True

        message_type = envelope_type or channel
        if message_type == "ack":
            self._ingest_farmely_reported(device_class, device_id, data, message_id)
            return True

        if channel == "event" or message_type == "event":
            self._ingest_farmely_event(device_class, device_id, data, message_id)
            return True

        if channel in {"status", "shadow"} or message_type == "status":
            self._ingest_farmely_reported(device_class, device_id, data, message_id)
            return True

        return True

    @staticmethod
    def _unwrap_envelope(
        document: object,
    ) -> tuple[str | None, str | None, dict[str, JsonValue] | None]:
        outer = _json_object(document)
        if outer is None:
            return None, None, None
        message_id = _message_id(outer)
        data = _json_object(outer.get("data"))
        if data is not None:
            message_type = outer.get("type")
            return message_type if isinstance(message_type, str) else None, message_id, data
        return None, message_id, outer

    def _ingest_farmely_reported(
        self,
        device_class: str,
        device_id: str,
        data: dict[str, JsonValue],
        message_id: str | None,
    ) -> None:
        device_type = data.get("type")
        if not isinstance(device_type, str):
            device_type = "light_bulb" if device_class == "light" else device_class
        name = data.get("name")
        if not isinstance(name, str):
            name = None
        capabilities = _string_list(data.get("capabilities"))
        metadata: dict[str, JsonValue] = {
            "class": device_class,
            "protocol": "farmely.v1",
            "source": "mqtt",
        }
        self._store.register_device(
            device_id,
            device_type,
            name,
            metadata,
            capabilities=capabilities,
        )

        reported = dict(data)
        reported.pop("capabilities", None)
        reported["type"] = device_type
        if "on" not in reported and isinstance(reported.get("state"), str):
            reported["on"] = reported["state"] == "on"
        reason_value = data.get("offline_reason")
        reason = reason_value if isinstance(reason_value, str) else "mqtt_disconnect"
        self._apply_reported(
            device_id,
            reported,
            message_id=message_id,
            offline_reason=None if reported.get("online") is True else reason,
        )

    def _ingest_farmely_event(
        self,
        device_class: str,
        device_id: str,
        data: dict[str, JsonValue],
        message_id: str | None,
    ) -> None:
        device_type = data.get("type")
        if not isinstance(device_type, str):
            device_type = device_class
        name = data.get("name")
        if not isinstance(name, str):
            name = None
        capabilities = _string_list(data.get("capabilities"))
        self._store.register_device(
            device_id,
            device_type,
            name,
            {"class": device_class, "protocol": "farmely.v1", "source": "mqtt"},
            capabilities=capabilities,
        )

        event_value = data.get("event", data.get("last_event"))
        if not isinstance(event_value, str):
            event_value = "event"
        reported = dict(data)
        reported.pop("capabilities", None)
        reported["online"] = True
        reported["type"] = device_type
        reported["last_event"] = event_value
        self._apply_reported(
            device_id,
            reported,
            message_id=message_id,
            offline_reason=None,
        )
        self._store.record_event(device_id, event_value, reported, message_id=message_id)

    def _ingest_node(self, node_id: str, document: dict) -> None:
        device_type = document.get("type")
        if not isinstance(device_type, str):
            device_type = "light_bulb"
        name = document.get("name")
        if not isinstance(name, str):
            name = None
        metadata: dict[str, JsonValue] = {"source": "mesh"}
        for key in ("role", "layer"):
            value = _scalar(document.get(key))
            if value is not None:
                metadata[key] = value
        self._store.register_device(
            node_id,
            device_type,
            name,
            metadata,
            capabilities=_string_list(document.get("capabilities")),
        )

        reported: dict[str, JsonValue] = {"type": device_type}
        for key in ("version", "online", "state", "on", "layer", "role", "name", "value"):
            value = _scalar(document.get(key))
            if value is not None:
                reported[key] = value
        if "on" not in reported and isinstance(reported.get("state"), str):
            reported["on"] = reported["state"] == "on"

        message_id = _message_id(_json_object(document) or {})
        online = reported.get("online")
        reason_value = document.get("offline_reason")
        reason = reason_value if isinstance(reason_value, str) else "gateway_reported_offline"
        self._apply_reported(
            node_id,
            reported,
            message_id=message_id,
            offline_reason=None if online is True else reason,
        )

    def _ingest_gateway(self, document: dict) -> None:
        root = document.get("root")
        gateway_id = root if isinstance(root, str) and root else "mesh-gateway"
        self._store.register_device(gateway_id, "gateway", "Mesh 网关", {"source": "mesh"})
        reported: dict[str, JsonValue] = {"type": "gateway"}
        for key in ("version", "online", "root", "layer", "nodes", "online_nodes", "heap"):
            value = _scalar(document.get(key))
            if value is not None:
                reported[key] = value
        message_id = _message_id(_json_object(document) or {})
        reason_value = document.get("offline_reason")
        reason = reason_value if isinstance(reason_value, str) else "mqtt_disconnect"
        self._apply_reported(
            gateway_id,
            reported,
            message_id=message_id,
            offline_reason=None if reported.get("online") is True else reason,
        )

    def _ingest_doorbell_status(self, doorbell_id: str, document: dict) -> None:
        device_type = document.get("type")
        if not isinstance(device_type, str):
            device_type = "doorbell"
        name = document.get("name")
        if not isinstance(name, str):
            name = "智能门铃"
        self._store.register_device(
            doorbell_id,
            device_type,
            name,
            {"source": "doorbell"},
            capabilities=_string_list(document.get("capabilities")),
        )
        reported: dict[str, JsonValue] = {"type": device_type}
        for key in ("version", "online", "name"):
            value = _scalar(document.get(key))
            if value is not None:
                reported[key] = value
        message_id = _message_id(_json_object(document) or {})
        reason_value = document.get("offline_reason")
        reason = reason_value if isinstance(reason_value, str) else "mqtt_disconnect"
        self._apply_reported(
            doorbell_id,
            reported,
            message_id=message_id,
            offline_reason=None if reported.get("online") is True else reason,
        )

    def _ingest_doorbell(self, doorbell_id: str, document: object) -> None:
        event: str | None = None
        message_id: str | None = None
        version: JsonValue | None = None
        if isinstance(document, str):
            event = document
        elif isinstance(document, dict):
            event_value = document.get("event")
            if isinstance(event_value, str):
                event = event_value
            message_value = document.get("message_id")
            if isinstance(message_value, str):
                message_id = message_value
            msg_value = document.get("msg_id")
            if isinstance(msg_value, str):
                message_id = msg_value
            version = _scalar(document.get("version"))
        if event is None:
            return
        self._store.register_device(doorbell_id, "doorbell", "智能门铃", {"source": "doorbell"})
        if event == "ack" and isinstance(document, dict):
            ack_message_id = document.get("ack_msg_id")
            if isinstance(ack_message_id, str):
                self._apply_reported(
                    doorbell_id,
                    {"online": True, "type": "doorbell", "ack_msg_id": ack_message_id},
                    message_id=message_id,
                    offline_reason=None,
                )
            return
        reported: dict[str, JsonValue] = {"online": True, "type": "doorbell", "last_event": event}
        if version is not None:
            reported["version"] = version
        self._apply_reported(
            doorbell_id,
            reported,
            message_id=message_id,
            offline_reason=None,
        )
        payload: dict[str, JsonValue] = {"event": event}
        if version is not None:
            payload["version"] = version
        self._store.record_event(doorbell_id, event, payload, message_id=message_id)
