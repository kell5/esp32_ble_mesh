from __future__ import annotations

import json
import sqlite3
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterator

from pydantic import JsonValue

from .models import DeviceResponse, ShadowResponse


class DeviceNotFoundError(LookupError):
    pass


class DeviceAlreadyClaimedError(RuntimeError):
    pass


def _utc_now() -> datetime:
    return datetime.now(timezone.utc)


def _dump_object(value: dict[str, JsonValue]) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def _load_object(raw: str) -> dict[str, JsonValue]:
    value = json.loads(raw)
    if not isinstance(value, dict):
        raise ValueError("stored JSON must be an object")
    return value


def _merge_object(
    current: dict[str, JsonValue], patch: dict[str, JsonValue]
) -> dict[str, JsonValue]:
    merged = dict(current)
    for key, value in patch.items():
        if value is None:
            merged.pop(key, None)
            continue
        existing = merged.get(key)
        if isinstance(existing, dict) and isinstance(value, dict):
            merged[key] = _merge_object(existing, value)
        else:
            merged[key] = value
    return merged


class DeviceStore:
    def __init__(self, database_path: Path) -> None:
        self._database_path = database_path
        self._database_path.parent.mkdir(parents=True, exist_ok=True)
        self.initialize()

    def _connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self._database_path, timeout=5)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys = ON")
        return connection

    @contextmanager
    def _connection(self) -> Iterator[sqlite3.Connection]:
        connection = self._connect()
        try:
            with connection:
                yield connection
        finally:
            connection.close()

    def initialize(self) -> None:
        with self._connection() as connection:
            connection.executescript(
                """
                PRAGMA journal_mode = WAL;
                CREATE TABLE IF NOT EXISTS devices (
                    device_id TEXT PRIMARY KEY,
                    owner_id TEXT,
                    type TEXT NOT NULL,
                    name TEXT,
                    metadata_json TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE INDEX IF NOT EXISTS idx_devices_owner ON devices(owner_id);

                CREATE TABLE IF NOT EXISTS device_shadows (
                    device_id TEXT PRIMARY KEY REFERENCES devices(device_id) ON DELETE CASCADE,
                    desired_json TEXT NOT NULL,
                    reported_json TEXT NOT NULL,
                    desired_version INTEGER NOT NULL DEFAULT 0,
                    reported_version INTEGER NOT NULL DEFAULT 0,
                    version INTEGER NOT NULL DEFAULT 0,
                    offline_reason TEXT,
                    last_seen_at TEXT,
                    updated_at TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS processed_messages (
                    scope TEXT NOT NULL,
                    message_id TEXT NOT NULL,
                    device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
                    created_at TEXT NOT NULL,
                    PRIMARY KEY(scope, message_id)
                );
                """
            )
            columns = {
                str(row["name"])
                for row in connection.execute("PRAGMA table_info(processed_messages)")
            }
            if "scope" not in columns:
                connection.executescript(
                    """
                    ALTER TABLE processed_messages RENAME TO processed_messages_legacy;
                    CREATE TABLE processed_messages (
                        scope TEXT NOT NULL,
                        message_id TEXT NOT NULL,
                        device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
                        created_at TEXT NOT NULL,
                        PRIMARY KEY(scope, message_id)
                    );
                    INSERT INTO processed_messages(scope, message_id, device_id, created_at)
                    SELECT 'legacy', message_id, device_id, created_at
                    FROM processed_messages_legacy;
                    DROP TABLE processed_messages_legacy;
                    """
                )

    def register_device(
        self,
        device_id: str,
        device_type: str,
        name: str | None = None,
        metadata: dict[str, JsonValue] | None = None,
    ) -> DeviceResponse:
        now = _utc_now().isoformat()
        metadata_patch = metadata or {}
        with self._connection() as connection:
            row = connection.execute(
                "SELECT metadata_json FROM devices WHERE device_id = ?", (device_id,)
            ).fetchone()
            if row is None:
                connection.execute(
                    """
                    INSERT INTO devices(
                        device_id, owner_id, type, name, metadata_json, created_at, updated_at
                    ) VALUES (?, NULL, ?, ?, ?, ?, ?)
                    """,
                    (device_id, device_type, name, _dump_object(metadata_patch), now, now),
                )
                connection.execute(
                    """
                    INSERT INTO device_shadows(
                        device_id, desired_json, reported_json, updated_at
                    ) VALUES (?, '{}', '{}', ?)
                    """,
                    (device_id, now),
                )
            else:
                merged_metadata = _merge_object(
                    _load_object(str(row["metadata_json"])), metadata_patch
                )
                connection.execute(
                    """
                    UPDATE devices
                    SET type = ?, name = COALESCE(?, name), metadata_json = ?, updated_at = ?
                    WHERE device_id = ?
                    """,
                    (device_type, name, _dump_object(merged_metadata), now, device_id),
                )
            return self._get_device(connection, device_id)

    def claim_device(self, device_id: str, user_id: str) -> DeviceResponse:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            row = connection.execute(
                "SELECT owner_id FROM devices WHERE device_id = ?", (device_id,)
            ).fetchone()
            if row is None:
                raise DeviceNotFoundError(device_id)
            owner_id = row["owner_id"]
            if owner_id is not None and owner_id != user_id:
                raise DeviceAlreadyClaimedError(device_id)
            connection.execute(
                "UPDATE devices SET owner_id = ?, updated_at = ? WHERE device_id = ?",
                (user_id, now, device_id),
            )
            return self._get_device(connection, device_id)

    def get_device(self, device_id: str) -> DeviceResponse:
        with self._connection() as connection:
            return self._get_device(connection, device_id)

    def list_devices(self, owner_id: str) -> list[DeviceResponse]:
        with self._connection() as connection:
            rows = connection.execute(
                "SELECT * FROM devices WHERE owner_id = ? ORDER BY created_at, device_id",
                (owner_id,),
            ).fetchall()
            return [self._device_from_row(row) for row in rows]

    def get_shadow(self, device_id: str) -> ShadowResponse:
        with self._connection() as connection:
            return self._get_shadow(connection, device_id)

    def update_desired(
        self, device_id: str, patch: dict[str, JsonValue], message_id: str | None
    ) -> tuple[ShadowResponse, bool]:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            current = self._get_shadow(connection, device_id)
            if message_id is not None and self._message_processed(
                connection, "desired", message_id
            ):
                return current, False
            merged = _merge_object(current.desired, patch)
            connection.execute(
                """
                UPDATE device_shadows
                SET desired_json = ?, desired_version = desired_version + 1,
                    version = version + 1, updated_at = ?
                WHERE device_id = ?
                """,
                (_dump_object(merged), now, device_id),
            )
            self._record_message(connection, "desired", device_id, message_id, now)
            return self._get_shadow(connection, device_id), True

    def update_reported(
        self,
        device_id: str,
        patch: dict[str, JsonValue],
        message_id: str | None = None,
        offline_reason: str | None = None,
    ) -> tuple[ShadowResponse, bool]:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            current = self._get_shadow(connection, device_id)
            if message_id is not None and self._message_processed(
                connection, "reported", message_id
            ):
                return current, False
            merged = _merge_object(current.reported, patch)
            online = merged.get("online")
            reason = None if online is True else offline_reason or current.offline_reason
            connection.execute(
                """
                UPDATE device_shadows
                SET reported_json = ?, reported_version = reported_version + 1,
                    version = version + 1, offline_reason = ?, last_seen_at = ?, updated_at = ?
                WHERE device_id = ?
                """,
                (_dump_object(merged), reason, now, now, device_id),
            )
            self._record_message(connection, "reported", device_id, message_id, now)
            return self._get_shadow(connection, device_id), True

    def mark_offline(self, device_id: str, reason: str) -> ShadowResponse:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            current = self._get_shadow(connection, device_id)
            reported = _merge_object(current.reported, {"online": False})
            connection.execute(
                """
                UPDATE device_shadows
                SET reported_json = ?, reported_version = reported_version + 1,
                    version = version + 1, offline_reason = ?, updated_at = ?
                WHERE device_id = ?
                """,
                (_dump_object(reported), reason, now, device_id),
            )
            return self._get_shadow(connection, device_id)

    def mark_stale_devices(self, cutoff: datetime) -> list[str]:
        marked: list[str] = []
        now = _utc_now().isoformat()
        with self._connection() as connection:
            rows = connection.execute(
                "SELECT * FROM device_shadows WHERE last_seen_at IS NOT NULL"
            ).fetchall()
            for row in rows:
                last_seen = datetime.fromisoformat(str(row["last_seen_at"]))
                reported = _load_object(str(row["reported_json"]))
                if last_seen >= cutoff or reported.get("online") is not True:
                    continue
                device_id = str(row["device_id"])
                reported["online"] = False
                connection.execute(
                    """
                    UPDATE device_shadows
                    SET reported_json = ?, reported_version = reported_version + 1,
                        version = version + 1, offline_reason = 'cloud_timeout', updated_at = ?
                    WHERE device_id = ?
                    """,
                    (_dump_object(reported), now, device_id),
                )
                marked.append(device_id)
        return marked

    def _get_device(self, connection: sqlite3.Connection, device_id: str) -> DeviceResponse:
        row = connection.execute(
            "SELECT * FROM devices WHERE device_id = ?", (device_id,)
        ).fetchone()
        if row is None:
            raise DeviceNotFoundError(device_id)
        return self._device_from_row(row)

    def _get_shadow(self, connection: sqlite3.Connection, device_id: str) -> ShadowResponse:
        row = connection.execute(
            "SELECT * FROM device_shadows WHERE device_id = ?", (device_id,)
        ).fetchone()
        if row is None:
            raise DeviceNotFoundError(device_id)
        return self._shadow_from_row(row)

    @staticmethod
    def _message_processed(connection: sqlite3.Connection, scope: str, message_id: str) -> bool:
        row = connection.execute(
            "SELECT 1 FROM processed_messages WHERE scope = ? AND message_id = ?",
            (scope, message_id),
        ).fetchone()
        return row is not None

    @staticmethod
    def _record_message(
        connection: sqlite3.Connection,
        scope: str,
        device_id: str,
        message_id: str | None,
        now: str,
    ) -> None:
        if message_id is None:
            return
        connection.execute(
            """
            INSERT INTO processed_messages(scope, message_id, device_id, created_at)
            VALUES (?, ?, ?, ?)
            """,
            (scope, message_id, device_id, now),
        )

    @staticmethod
    def _device_from_row(row: sqlite3.Row) -> DeviceResponse:
        owner_value = row["owner_id"]
        name_value = row["name"]
        return DeviceResponse(
            device_id=str(row["device_id"]),
            owner_id=None if owner_value is None else str(owner_value),
            type=str(row["type"]),
            name=None if name_value is None else str(name_value),
            metadata=_load_object(str(row["metadata_json"])),
            created_at=datetime.fromisoformat(str(row["created_at"])),
            updated_at=datetime.fromisoformat(str(row["updated_at"])),
        )

    @staticmethod
    def _shadow_from_row(row: sqlite3.Row) -> ShadowResponse:
        offline_value = row["offline_reason"]
        last_seen_value = row["last_seen_at"]
        return ShadowResponse(
            device_id=str(row["device_id"]),
            desired=_load_object(str(row["desired_json"])),
            reported=_load_object(str(row["reported_json"])),
            desired_version=int(row["desired_version"]),
            reported_version=int(row["reported_version"]),
            version=int(row["version"]),
            offline_reason=None if offline_value is None else str(offline_value),
            last_seen_at=(
                None if last_seen_value is None else datetime.fromisoformat(str(last_seen_value))
            ),
            updated_at=datetime.fromisoformat(str(row["updated_at"])),
        )
