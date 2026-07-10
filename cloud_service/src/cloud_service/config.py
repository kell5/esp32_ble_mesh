from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


def _env_bool(name: str, default: bool) -> bool:
    raw = os.getenv(name)
    if raw is None:
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


@dataclass(frozen=True)
class Settings:
    database_path: Path
    api_host: str
    api_port: int
    api_token: str | None
    mqtt_enabled: bool
    mqtt_host: str
    mqtt_port: int
    mqtt_username: str | None
    mqtt_password: str | None
    mqtt_client_id: str
    mqtt_node_status_filter: str
    mqtt_gateway_status_topic: str
    mqtt_doorbell_status_filter: str
    mqtt_doorbell_event_filter: str
    stale_after_seconds: int

    @classmethod
    def from_env(cls) -> Settings:
        return cls(
            database_path=Path(os.getenv("CLOUD_DATABASE_PATH", "data/cloud.db")),
            api_host=os.getenv("CLOUD_API_HOST", "127.0.0.1"),
            api_port=int(os.getenv("CLOUD_API_PORT", "8000")),
            api_token=os.getenv("CLOUD_API_TOKEN"),
            mqtt_enabled=_env_bool("CLOUD_MQTT_ENABLED", False),
            mqtt_host=os.getenv("CLOUD_MQTT_HOST", "127.0.0.1"),
            mqtt_port=int(os.getenv("CLOUD_MQTT_PORT", "1883")),
            mqtt_username=os.getenv("CLOUD_MQTT_USERNAME"),
            mqtt_password=os.getenv("CLOUD_MQTT_PASSWORD"),
            mqtt_client_id=os.getenv("CLOUD_MQTT_CLIENT_ID", "mesh-cloud-service"),
            mqtt_node_status_filter=os.getenv(
                "CLOUD_MQTT_NODE_STATUS_FILTER", "office/light/node/+/status"
            ),
            mqtt_gateway_status_topic=os.getenv(
                "CLOUD_MQTT_GATEWAY_STATUS_TOPIC", "office/light/gateway/status"
            ),
            mqtt_doorbell_status_filter=os.getenv(
                "CLOUD_MQTT_DOORBELL_STATUS_FILTER", "doorbell/+/status"
            ),
            mqtt_doorbell_event_filter=os.getenv(
                "CLOUD_MQTT_DOORBELL_EVENT_FILTER", "doorbell/+/event"
            ),
            stale_after_seconds=int(os.getenv("CLOUD_STALE_AFTER_SECONDS", "90")),
        )
