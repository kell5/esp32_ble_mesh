from __future__ import annotations

import json
from threading import Event

from paho.mqtt import client as mqtt
from pydantic import JsonValue

from .config import Settings
from .storage import DeviceStore


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


class MqttBridge:
    def __init__(self, store: DeviceStore, settings: Settings) -> None:
        self._store = store
        self._settings = settings
        self._client: mqtt.Client | None = None
        self._connected = Event()

    @property
    def connected(self) -> bool:
        return self._connected.is_set()

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
            topic = f"office/light/node/{device_id}/cmd"
            payload = "on" if on else "off"
        else:
            command = state.get("command")
            if not isinstance(command, str):
                return False
            topic = f"doorbell/{device_id}/cmd"
            payload = command

        result = client.publish(topic, payload, qos=1, retain=False)
        return result.rc == mqtt.MQTT_ERR_SUCCESS

    def ingest(self, topic: str, payload: bytes) -> None:
        text = payload.decode("utf-8", errors="replace")
        try:
            document = json.loads(text)
        except json.JSONDecodeError:
            document = text

        node_id = _topic_value(self._settings.mqtt_node_status_filter, topic)
        if node_id is not None and isinstance(document, dict):
            self._ingest_node(node_id, document)
            return

        if topic == self._settings.mqtt_gateway_status_topic and isinstance(document, dict):
            self._ingest_gateway(document)
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
                (self._settings.mqtt_node_status_filter, 1),
                (self._settings.mqtt_gateway_status_topic, 1),
                (self._settings.mqtt_doorbell_event_filter, 1),
            ]
        )

    def _on_disconnect(self, _client, _userdata, _flags, _reason_code, _properties) -> None:
        self._connected.clear()

    def _on_message(self, _client, _userdata, message: mqtt.MQTTMessage) -> None:
        self.ingest(message.topic, message.payload)

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
        self._store.register_device(node_id, device_type, name, metadata)

        reported: dict[str, JsonValue] = {"type": device_type}
        for key in ("online", "state", "on", "layer", "role", "name", "value"):
            value = _scalar(document.get(key))
            if value is not None:
                reported[key] = value
        if "on" not in reported and isinstance(reported.get("state"), str):
            reported["on"] = reported["state"] == "on"

        message_id = document.get("message_id")
        if not isinstance(message_id, str):
            message_id = None
        online = reported.get("online")
        reason_value = document.get("offline_reason")
        reason = reason_value if isinstance(reason_value, str) else "gateway_reported_offline"
        self._store.update_reported(
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
        for key in ("online", "root", "layer", "nodes", "online_nodes", "heap"):
            value = _scalar(document.get(key))
            if value is not None:
                reported[key] = value
        message_id = document.get("message_id")
        if not isinstance(message_id, str):
            message_id = None
        reason_value = document.get("offline_reason")
        reason = reason_value if isinstance(reason_value, str) else "mqtt_disconnect"
        self._store.update_reported(
            gateway_id,
            reported,
            message_id=message_id,
            offline_reason=None if reported.get("online") is True else reason,
        )

    def _ingest_doorbell(self, doorbell_id: str, document: object) -> None:
        event: str | None = None
        message_id: str | None = None
        if isinstance(document, str):
            event = document
        elif isinstance(document, dict):
            event_value = document.get("event")
            if isinstance(event_value, str):
                event = event_value
            message_value = document.get("message_id")
            if isinstance(message_value, str):
                message_id = message_value
        if event is None:
            return
        self._store.register_device(doorbell_id, "doorbell", "智能门铃", {"source": "doorbell"})
        self._store.update_reported(
            doorbell_id,
            {"online": True, "type": "doorbell", "last_event": event},
            message_id=message_id,
        )
