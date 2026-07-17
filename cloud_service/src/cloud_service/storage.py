from __future__ import annotations

import hashlib
import json
import sqlite3
import uuid
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterator

from pydantic import JsonValue

from .models import (
    AccountResponse,
    AutomationAction,
    AutomationResponse,
    AutomationTrigger,
    DeviceEventResponse,
    DeviceResponse,
    FirmwareResponse,
    GroupResponse,
    OtaUpdateRecord,
    RolloutResponse,
    RoomResponse,
    SceneAction,
    SceneResponse,
    ShadowResponse,
)
from .security import hash_password, new_token, verify_password


class DeviceNotFoundError(LookupError):
    pass


class AccountNotFoundError(LookupError):
    pass


class EmailAlreadyExistsError(RuntimeError):
    pass


class InvalidCredentialsError(RuntimeError):
    pass


class DeviceAlreadyClaimedError(RuntimeError):
    pass


class DeviceNotOwnedError(RuntimeError):
    pass


class RoomNotFoundError(LookupError):
    pass


class GroupNotFoundError(LookupError):
    pass


class SceneNotFoundError(LookupError):
    pass


class AutomationNotFoundError(LookupError):
    pass


class FirmwareNotFoundError(LookupError):
    pass


class RolloutNotFoundError(LookupError):
    pass


class OtaUpdateNotFoundError(LookupError):
    pass


def _utc_now() -> datetime:
    return datetime.now(timezone.utc)


def _new_id(prefix: str) -> str:
    return f"{prefix}-{uuid.uuid4().hex}"


def _dump_object(value: dict[str, JsonValue]) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def _dump_string_list(value: list[str]) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def _load_object(raw: str) -> dict[str, JsonValue]:
    value = json.loads(raw)
    if not isinstance(value, dict):
        raise ValueError("stored JSON must be an object")
    return value


def _load_string_list(raw: str | None) -> list[str]:
    if raw is None:
        return []
    value = json.loads(raw)
    if not isinstance(value, list):
        return []
    return [item for item in value if isinstance(item, str)]


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
                CREATE TABLE IF NOT EXISTS rooms (
                    room_id TEXT PRIMARY KEY,
                    owner_id TEXT NOT NULL,
                    name TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE INDEX IF NOT EXISTS idx_rooms_owner ON rooms(owner_id);

                CREATE TABLE IF NOT EXISTS devices (
                    device_id TEXT PRIMARY KEY,
                    owner_id TEXT,
                    type TEXT NOT NULL,
                    name TEXT,
                    room_id TEXT REFERENCES rooms(room_id) ON DELETE SET NULL,
                    capabilities_json TEXT NOT NULL DEFAULT '[]',
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

                CREATE TABLE IF NOT EXISTS device_groups (
                    group_id TEXT PRIMARY KEY,
                    owner_id TEXT NOT NULL,
                    name TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE INDEX IF NOT EXISTS idx_groups_owner ON device_groups(owner_id);

                CREATE TABLE IF NOT EXISTS group_members (
                    group_id TEXT NOT NULL REFERENCES device_groups(group_id) ON DELETE CASCADE,
                    device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
                    PRIMARY KEY(group_id, device_id)
                );

                CREATE TABLE IF NOT EXISTS scenes (
                    scene_id TEXT PRIMARY KEY,
                    owner_id TEXT NOT NULL,
                    name TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE INDEX IF NOT EXISTS idx_scenes_owner ON scenes(owner_id);

                CREATE TABLE IF NOT EXISTS scene_actions (
                    scene_id TEXT NOT NULL REFERENCES scenes(scene_id) ON DELETE CASCADE,
                    device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
                    state_json TEXT NOT NULL,
                    PRIMARY KEY(scene_id, device_id)
                );

                CREATE TABLE IF NOT EXISTS automations (
                    automation_id TEXT PRIMARY KEY,
                    owner_id TEXT NOT NULL,
                    name TEXT NOT NULL,
                    enabled INTEGER NOT NULL DEFAULT 1,
                    trigger_device_id TEXT NOT NULL,
                    trigger_json TEXT NOT NULL,
                    action_json TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE INDEX IF NOT EXISTS idx_automations_owner ON automations(owner_id);
                CREATE INDEX IF NOT EXISTS idx_automations_trigger
                    ON automations(trigger_device_id);

                CREATE TABLE IF NOT EXISTS device_events (
                    event_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
                    event TEXT NOT NULL,
                    payload_json TEXT NOT NULL,
                    message_id TEXT,
                    created_at TEXT NOT NULL
                );
                CREATE INDEX IF NOT EXISTS idx_events_device
                    ON device_events(device_id, event_id);
                CREATE UNIQUE INDEX IF NOT EXISTS idx_events_message
                    ON device_events(device_id, message_id)
                    WHERE message_id IS NOT NULL;

                CREATE TABLE IF NOT EXISTS accounts (
                    user_id TEXT PRIMARY KEY,
                    email TEXT NOT NULL UNIQUE,
                    password_hash TEXT NOT NULL,
                    created_at TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS account_tokens (
                    token TEXT PRIMARY KEY,
                    user_id TEXT NOT NULL REFERENCES accounts(user_id) ON DELETE CASCADE,
                    created_at TEXT NOT NULL
                );
                CREATE INDEX IF NOT EXISTS idx_account_tokens_user
                    ON account_tokens(user_id);

                CREATE TABLE IF NOT EXISTS firmware (
                    firmware_id TEXT PRIMARY KEY,
                    product_id TEXT NOT NULL,
                    hw_version TEXT NOT NULL,
                    fw_version TEXT NOT NULL,
                    url TEXT NOT NULL,
                    sha256 TEXT NOT NULL,
                    sign TEXT,
                    notes TEXT,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL,
                    UNIQUE(product_id, hw_version, fw_version)
                );
                CREATE INDEX IF NOT EXISTS idx_firmware_target
                    ON firmware(product_id, hw_version, fw_version);

                CREATE TABLE IF NOT EXISTS rollouts (
                    rollout_id TEXT PRIMARY KEY,
                    product_id TEXT NOT NULL,
                    hw_version TEXT NOT NULL,
                    target_fw_version TEXT NOT NULL,
                    from_fw_version TEXT,
                    percent INTEGER NOT NULL DEFAULT 100,
                    enabled INTEGER NOT NULL DEFAULT 1,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE INDEX IF NOT EXISTS idx_rollouts_match
                    ON rollouts(product_id, hw_version);

                CREATE TABLE IF NOT EXISTS ota_updates (
                    update_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
                    firmware_id TEXT REFERENCES firmware(firmware_id) ON DELETE SET NULL,
                    product_id TEXT NOT NULL,
                    hw_version TEXT NOT NULL,
                    target_fw_version TEXT NOT NULL,
                    status TEXT NOT NULL,
                    message_id TEXT,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL,
                    UNIQUE(device_id, firmware_id)
                );
                CREATE INDEX IF NOT EXISTS idx_ota_updates_device
                    ON ota_updates(device_id, update_id);
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
            device_columns = {
                str(row["name"]) for row in connection.execute("PRAGMA table_info(devices)")
            }
            if "room_id" not in device_columns:
                connection.execute(
                    "ALTER TABLE devices ADD COLUMN room_id TEXT "
                    "REFERENCES rooms(room_id) ON DELETE SET NULL"
                )
            if "capabilities_json" not in device_columns:
                connection.execute(
                    "ALTER TABLE devices ADD COLUMN capabilities_json TEXT NOT NULL DEFAULT '[]'"
                )
            connection.execute(
                "CREATE INDEX IF NOT EXISTS idx_devices_room ON devices(room_id)"
            )

    def register_device(
        self,
        device_id: str,
        device_type: str,
        name: str | None = None,
        metadata: dict[str, JsonValue] | None = None,
        capabilities: list[str] | None = None,
    ) -> DeviceResponse:
        now = _utc_now().isoformat()
        metadata_patch = metadata or {}
        with self._connection() as connection:
            row = connection.execute(
                "SELECT metadata_json FROM devices WHERE device_id = ?", (device_id,)
            ).fetchone()
            if row is None:
                capabilities_list = capabilities if capabilities else []
                connection.execute(
                    """
                    INSERT INTO devices(
                        device_id, owner_id, type, name, metadata_json, capabilities_json,
                        created_at, updated_at
                    ) VALUES (?, NULL, ?, ?, ?, ?, ?, ?)
                    """,
                    (
                        device_id,
                        device_type,
                        name,
                        _dump_object(metadata_patch),
                        _dump_string_list(capabilities_list),
                        now,
                        now,
                    ),
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
                    SET type = ?,
                        name = COALESCE(?, name),
                        metadata_json = ?,
                        capabilities_json = COALESCE(?, capabilities_json),
                        updated_at = ?
                    WHERE device_id = ?
                    """,
                    (
                        device_type,
                        name,
                        _dump_object(merged_metadata),
                        None if capabilities is None else _dump_string_list(capabilities),
                        now,
                        device_id,
                    ),
                )
            return self._get_device(connection, device_id)

    def claim_device(
        self, device_id: str, user_id: str, force: bool = False
    ) -> DeviceResponse:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            row = connection.execute(
                "SELECT owner_id FROM devices WHERE device_id = ?", (device_id,)
            ).fetchone()
            if row is None:
                raise DeviceNotFoundError(device_id)
            owner_id = row["owner_id"]
            if not force and owner_id is not None and owner_id != user_id:
                raise DeviceAlreadyClaimedError(device_id)
            connection.execute(
                "UPDATE devices SET owner_id = ?, updated_at = ? WHERE device_id = ?",
                (user_id, now, device_id),
            )
            return self._get_device(connection, device_id)

    def unclaim_device(self, device_id: str, user_id: str) -> None:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            row = connection.execute(
                "SELECT owner_id FROM devices WHERE device_id = ?", (device_id,)
            ).fetchone()
            if row is None:
                raise DeviceNotFoundError(device_id)
            if row["owner_id"] != user_id:
                raise DeviceNotOwnedError(device_id)
            connection.execute(
                "UPDATE devices SET owner_id = NULL, updated_at = ? WHERE device_id = ?",
                (now, device_id),
            )

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

    # --- accounts ----------------------------------------------------------

    def create_account(self, email: str, password: str) -> AccountResponse:
        now = _utc_now().isoformat()
        user_id = _new_id("user")
        normalized = email.strip().lower()
        with self._connection() as connection:
            try:
                connection.execute(
                    """
                    INSERT INTO accounts(user_id, email, password_hash, created_at)
                    VALUES (?, ?, ?, ?)
                    """,
                    (user_id, normalized, hash_password(password), now),
                )
            except sqlite3.IntegrityError as error:
                raise EmailAlreadyExistsError(normalized) from error
            return self._account_from_row(
                connection.execute(
                    "SELECT * FROM accounts WHERE user_id = ?", (user_id,)
                ).fetchone()
            )

    def issue_token(self, user_id: str) -> str:
        now = _utc_now().isoformat()
        token = new_token()
        with self._connection() as connection:
            connection.execute(
                "INSERT INTO account_tokens(token, user_id, created_at) VALUES (?, ?, ?)",
                (token, user_id, now),
            )
        return token

    def authenticate(self, email: str, password: str) -> AccountResponse:
        normalized = email.strip().lower()
        with self._connection() as connection:
            row = connection.execute(
                "SELECT * FROM accounts WHERE email = ?", (normalized,)
            ).fetchone()
            if row is None or not verify_password(password, str(row["password_hash"])):
                raise InvalidCredentialsError(normalized)
            return self._account_from_row(row)

    def get_account(self, user_id: str) -> AccountResponse:
        with self._connection() as connection:
            row = connection.execute(
                "SELECT * FROM accounts WHERE user_id = ?", (user_id,)
            ).fetchone()
            if row is None:
                raise AccountNotFoundError(user_id)
            return self._account_from_row(row)

    def resolve_token(self, token: str) -> str | None:
        with self._connection() as connection:
            row = connection.execute(
                "SELECT user_id FROM account_tokens WHERE token = ?", (token,)
            ).fetchone()
            return None if row is None else str(row["user_id"])

    def revoke_token(self, token: str) -> None:
        with self._connection() as connection:
            connection.execute("DELETE FROM account_tokens WHERE token = ?", (token,))

    # --- events ------------------------------------------------------------

    def record_event(
        self,
        device_id: str,
        event: str,
        payload: dict[str, JsonValue],
        message_id: str | None = None,
    ) -> DeviceEventResponse | None:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            self._require_device(connection, device_id)
            try:
                cursor = connection.execute(
                    """
                    INSERT INTO device_events(
                        device_id, event, payload_json, message_id, created_at
                    ) VALUES (?, ?, ?, ?, ?)
                    """,
                    (device_id, event, _dump_object(payload), message_id, now),
                )
            except sqlite3.IntegrityError:
                return None
            return self._get_event(connection, int(cursor.lastrowid))

    def list_device_events(
        self, device_id: str, limit: int = 50, before_id: int | None = None
    ) -> list[DeviceEventResponse]:
        with self._connection() as connection:
            self._require_device(connection, device_id)
            if before_id is None:
                rows = connection.execute(
                    "SELECT * FROM device_events WHERE device_id = ? "
                    "ORDER BY event_id DESC LIMIT ?",
                    (device_id, limit),
                ).fetchall()
            else:
                rows = connection.execute(
                    "SELECT * FROM device_events WHERE device_id = ? AND event_id < ? "
                    "ORDER BY event_id DESC LIMIT ?",
                    (device_id, before_id, limit),
                ).fetchall()
            return [self._event_from_row(row) for row in rows]

    def list_owner_events(
        self, owner_id: str, limit: int = 50, before_id: int | None = None
    ) -> list[DeviceEventResponse]:
        with self._connection() as connection:
            if before_id is None:
                rows = connection.execute(
                    "SELECT e.* FROM device_events e "
                    "JOIN devices d ON d.device_id = e.device_id "
                    "WHERE d.owner_id = ? ORDER BY e.event_id DESC LIMIT ?",
                    (owner_id, limit),
                ).fetchall()
            else:
                rows = connection.execute(
                    "SELECT e.* FROM device_events e "
                    "JOIN devices d ON d.device_id = e.device_id "
                    "WHERE d.owner_id = ? AND e.event_id < ? "
                    "ORDER BY e.event_id DESC LIMIT ?",
                    (owner_id, before_id, limit),
                ).fetchall()
            return [self._event_from_row(row) for row in rows]

    # --- rooms -------------------------------------------------------------

    def create_room(self, owner_id: str, name: str) -> RoomResponse:
        now = _utc_now().isoformat()
        room_id = _new_id("room")
        with self._connection() as connection:
            connection.execute(
                """
                INSERT INTO rooms(room_id, owner_id, name, created_at, updated_at)
                VALUES (?, ?, ?, ?, ?)
                """,
                (room_id, owner_id, name, now, now),
            )
            return self._get_room(connection, room_id)

    def list_rooms(self, owner_id: str) -> list[RoomResponse]:
        with self._connection() as connection:
            rows = connection.execute(
                "SELECT room_id FROM rooms WHERE owner_id = ? ORDER BY created_at, room_id",
                (owner_id,),
            ).fetchall()
            return [self._get_room(connection, str(row["room_id"])) for row in rows]

    def get_room(self, room_id: str) -> RoomResponse:
        with self._connection() as connection:
            return self._get_room(connection, room_id)

    def rename_room(self, room_id: str, name: str) -> RoomResponse:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            self._require_room(connection, room_id)
            connection.execute(
                "UPDATE rooms SET name = ?, updated_at = ? WHERE room_id = ?",
                (name, now, room_id),
            )
            return self._get_room(connection, room_id)

    def delete_room(self, room_id: str) -> None:
        with self._connection() as connection:
            self._require_room(connection, room_id)
            connection.execute("DELETE FROM rooms WHERE room_id = ?", (room_id,))

    def assign_device_to_room(self, room_id: str, device_id: str) -> RoomResponse:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            self._require_room(connection, room_id)
            self._require_device(connection, device_id)
            connection.execute(
                "UPDATE devices SET room_id = ?, updated_at = ? WHERE device_id = ?",
                (room_id, now, device_id),
            )
            return self._get_room(connection, room_id)

    def remove_device_from_room(self, room_id: str, device_id: str) -> RoomResponse:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            self._require_room(connection, room_id)
            self._require_device(connection, device_id)
            connection.execute(
                "UPDATE devices SET room_id = NULL, updated_at = ? "
                "WHERE device_id = ? AND room_id = ?",
                (now, device_id, room_id),
            )
            return self._get_room(connection, room_id)

    # --- groups ------------------------------------------------------------

    def create_group(self, owner_id: str, name: str) -> GroupResponse:
        now = _utc_now().isoformat()
        group_id = _new_id("group")
        with self._connection() as connection:
            connection.execute(
                """
                INSERT INTO device_groups(group_id, owner_id, name, created_at, updated_at)
                VALUES (?, ?, ?, ?, ?)
                """,
                (group_id, owner_id, name, now, now),
            )
            return self._get_group(connection, group_id)

    def list_groups(self, owner_id: str) -> list[GroupResponse]:
        with self._connection() as connection:
            rows = connection.execute(
                "SELECT group_id FROM device_groups WHERE owner_id = ? "
                "ORDER BY created_at, group_id",
                (owner_id,),
            ).fetchall()
            return [self._get_group(connection, str(row["group_id"])) for row in rows]

    def get_group(self, group_id: str) -> GroupResponse:
        with self._connection() as connection:
            return self._get_group(connection, group_id)

    def rename_group(self, group_id: str, name: str) -> GroupResponse:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            self._require_group(connection, group_id)
            connection.execute(
                "UPDATE device_groups SET name = ?, updated_at = ? WHERE group_id = ?",
                (name, now, group_id),
            )
            return self._get_group(connection, group_id)

    def delete_group(self, group_id: str) -> None:
        with self._connection() as connection:
            self._require_group(connection, group_id)
            connection.execute("DELETE FROM device_groups WHERE group_id = ?", (group_id,))

    def add_group_member(self, group_id: str, device_id: str) -> GroupResponse:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            self._require_group(connection, group_id)
            self._require_device(connection, device_id)
            connection.execute(
                "INSERT OR IGNORE INTO group_members(group_id, device_id) VALUES (?, ?)",
                (group_id, device_id),
            )
            connection.execute(
                "UPDATE device_groups SET updated_at = ? WHERE group_id = ?",
                (now, group_id),
            )
            return self._get_group(connection, group_id)

    def remove_group_member(self, group_id: str, device_id: str) -> GroupResponse:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            self._require_group(connection, group_id)
            connection.execute(
                "DELETE FROM group_members WHERE group_id = ? AND device_id = ?",
                (group_id, device_id),
            )
            connection.execute(
                "UPDATE device_groups SET updated_at = ? WHERE group_id = ?",
                (now, group_id),
            )
            return self._get_group(connection, group_id)

    def list_group_member_ids(self, group_id: str) -> list[str]:
        with self._connection() as connection:
            self._require_group(connection, group_id)
            return self._group_member_ids(connection, group_id)

    # --- scenes ------------------------------------------------------------

    def create_scene(
        self, owner_id: str, name: str, actions: list[SceneAction]
    ) -> SceneResponse:
        now = _utc_now().isoformat()
        scene_id = _new_id("scene")
        with self._connection() as connection:
            connection.execute(
                """
                INSERT INTO scenes(scene_id, owner_id, name, created_at, updated_at)
                VALUES (?, ?, ?, ?, ?)
                """,
                (scene_id, owner_id, name, now, now),
            )
            self._replace_scene_actions(connection, scene_id, actions)
            return self._get_scene(connection, scene_id)

    def list_scenes(self, owner_id: str) -> list[SceneResponse]:
        with self._connection() as connection:
            rows = connection.execute(
                "SELECT scene_id FROM scenes WHERE owner_id = ? ORDER BY created_at, scene_id",
                (owner_id,),
            ).fetchall()
            return [self._get_scene(connection, str(row["scene_id"])) for row in rows]

    def get_scene(self, scene_id: str) -> SceneResponse:
        with self._connection() as connection:
            return self._get_scene(connection, scene_id)

    def update_scene(
        self,
        scene_id: str,
        name: str | None,
        actions: list[SceneAction] | None,
    ) -> SceneResponse:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            self._require_scene(connection, scene_id)
            if name is not None:
                connection.execute(
                    "UPDATE scenes SET name = ? WHERE scene_id = ?", (name, scene_id)
                )
            if actions is not None:
                self._replace_scene_actions(connection, scene_id, actions)
            connection.execute(
                "UPDATE scenes SET updated_at = ? WHERE scene_id = ?", (now, scene_id)
            )
            return self._get_scene(connection, scene_id)

    def delete_scene(self, scene_id: str) -> None:
        with self._connection() as connection:
            self._require_scene(connection, scene_id)
            connection.execute("DELETE FROM scenes WHERE scene_id = ?", (scene_id,))

    def get_scene_actions(self, scene_id: str) -> list[SceneAction]:
        with self._connection() as connection:
            self._require_scene(connection, scene_id)
            return self._scene_actions(connection, scene_id)

    # --- automations -------------------------------------------------------

    def create_automation(
        self,
        owner_id: str,
        name: str,
        enabled: bool,
        trigger: AutomationTrigger,
        action: AutomationAction,
    ) -> AutomationResponse:
        now = _utc_now().isoformat()
        automation_id = _new_id("automation")
        with self._connection() as connection:
            connection.execute(
                """
                INSERT INTO automations(
                    automation_id, owner_id, name, enabled, trigger_device_id,
                    trigger_json, action_json, created_at, updated_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    automation_id,
                    owner_id,
                    name,
                    1 if enabled else 0,
                    trigger.device_id,
                    trigger.model_dump_json(),
                    action.model_dump_json(),
                    now,
                    now,
                ),
            )
            return self._get_automation(connection, automation_id)

    def list_automations(self, owner_id: str) -> list[AutomationResponse]:
        with self._connection() as connection:
            rows = connection.execute(
                "SELECT automation_id FROM automations WHERE owner_id = ? "
                "ORDER BY created_at, automation_id",
                (owner_id,),
            ).fetchall()
            return [
                self._get_automation(connection, str(row["automation_id"])) for row in rows
            ]

    def get_automation(self, automation_id: str) -> AutomationResponse:
        with self._connection() as connection:
            return self._get_automation(connection, automation_id)

    def update_automation(
        self,
        automation_id: str,
        name: str | None,
        enabled: bool | None,
        trigger: AutomationTrigger | None,
        action: AutomationAction | None,
    ) -> AutomationResponse:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            self._require_automation(connection, automation_id)
            if name is not None:
                connection.execute(
                    "UPDATE automations SET name = ? WHERE automation_id = ?",
                    (name, automation_id),
                )
            if enabled is not None:
                connection.execute(
                    "UPDATE automations SET enabled = ? WHERE automation_id = ?",
                    (1 if enabled else 0, automation_id),
                )
            if trigger is not None:
                connection.execute(
                    "UPDATE automations SET trigger_device_id = ?, trigger_json = ? "
                    "WHERE automation_id = ?",
                    (trigger.device_id, trigger.model_dump_json(), automation_id),
                )
            if action is not None:
                connection.execute(
                    "UPDATE automations SET action_json = ? WHERE automation_id = ?",
                    (action.model_dump_json(), automation_id),
                )
            connection.execute(
                "UPDATE automations SET updated_at = ? WHERE automation_id = ?",
                (now, automation_id),
            )
            return self._get_automation(connection, automation_id)

    def delete_automation(self, automation_id: str) -> None:
        with self._connection() as connection:
            self._require_automation(connection, automation_id)
            connection.execute(
                "DELETE FROM automations WHERE automation_id = ?", (automation_id,)
            )

    def list_enabled_automations_for(self, device_id: str) -> list[AutomationResponse]:
        with self._connection() as connection:
            rows = connection.execute(
                "SELECT automation_id FROM automations "
                "WHERE trigger_device_id = ? AND enabled = 1 "
                "ORDER BY created_at, automation_id",
                (device_id,),
            ).fetchall()
            return [
                self._get_automation(connection, str(row["automation_id"])) for row in rows
            ]

    # --- ota ---------------------------------------------------------------

    def register_firmware(
        self,
        product_id: str,
        hw_version: str,
        fw_version: str,
        url: str,
        sha256: str,
        sign: str | None = None,
        notes: str | None = None,
    ) -> FirmwareResponse:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            row = connection.execute(
                """
                SELECT firmware_id FROM firmware
                WHERE product_id = ? AND hw_version = ? AND fw_version = ?
                """,
                (product_id, hw_version, fw_version),
            ).fetchone()
            if row is None:
                firmware_id = _new_id("fw")
                connection.execute(
                    """
                    INSERT INTO firmware(
                        firmware_id, product_id, hw_version, fw_version, url, sha256,
                        sign, notes, created_at, updated_at
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    """,
                    (
                        firmware_id, product_id, hw_version, fw_version, url, sha256,
                        sign, notes, now, now,
                    ),
                )
            else:
                firmware_id = str(row["firmware_id"])
                connection.execute(
                    """
                    UPDATE firmware
                    SET url = ?, sha256 = ?, sign = ?, notes = ?, updated_at = ?
                    WHERE firmware_id = ?
                    """,
                    (url, sha256, sign, notes, now, firmware_id),
                )
            return self._get_firmware(connection, firmware_id)

    def list_firmware(self, product_id: str | None = None) -> list[FirmwareResponse]:
        with self._connection() as connection:
            if product_id is None:
                rows = connection.execute(
                    "SELECT * FROM firmware ORDER BY created_at, firmware_id"
                ).fetchall()
            else:
                rows = connection.execute(
                    "SELECT * FROM firmware WHERE product_id = ? "
                    "ORDER BY created_at, firmware_id",
                    (product_id,),
                ).fetchall()
            return [self._firmware_from_row(row) for row in rows]

    def get_firmware(self, firmware_id: str) -> FirmwareResponse:
        with self._connection() as connection:
            return self._get_firmware(connection, firmware_id)

    def delete_firmware(self, firmware_id: str) -> None:
        with self._connection() as connection:
            self._require_firmware(connection, firmware_id)
            connection.execute(
                "DELETE FROM firmware WHERE firmware_id = ?", (firmware_id,)
            )

    def create_rollout(
        self,
        product_id: str,
        hw_version: str,
        target_fw_version: str,
        from_fw_version: str | None,
        percent: int,
        enabled: bool,
    ) -> RolloutResponse:
        now = _utc_now().isoformat()
        rollout_id = _new_id("rollout")
        with self._connection() as connection:
            connection.execute(
                """
                INSERT INTO rollouts(
                    rollout_id, product_id, hw_version, target_fw_version,
                    from_fw_version, percent, enabled, created_at, updated_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    rollout_id, product_id, hw_version, target_fw_version,
                    from_fw_version, percent, 1 if enabled else 0, now, now,
                ),
            )
            return self._get_rollout(connection, rollout_id)

    def list_rollouts(self) -> list[RolloutResponse]:
        with self._connection() as connection:
            rows = connection.execute(
                "SELECT rollout_id FROM rollouts ORDER BY created_at, rollout_id"
            ).fetchall()
            return [self._get_rollout(connection, str(row["rollout_id"])) for row in rows]

    def get_rollout(self, rollout_id: str) -> RolloutResponse:
        with self._connection() as connection:
            return self._get_rollout(connection, rollout_id)

    def update_rollout(
        self,
        rollout_id: str,
        target_fw_version: str | None,
        from_fw_version: str | None,
        percent: int | None,
        enabled: bool | None,
    ) -> RolloutResponse:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            self._require_rollout(connection, rollout_id)
            if target_fw_version is not None:
                connection.execute(
                    "UPDATE rollouts SET target_fw_version = ? WHERE rollout_id = ?",
                    (target_fw_version, rollout_id),
                )
            if from_fw_version is not None:
                connection.execute(
                    "UPDATE rollouts SET from_fw_version = ? WHERE rollout_id = ?",
                    (from_fw_version, rollout_id),
                )
            if percent is not None:
                connection.execute(
                    "UPDATE rollouts SET percent = ? WHERE rollout_id = ?",
                    (percent, rollout_id),
                )
            if enabled is not None:
                connection.execute(
                    "UPDATE rollouts SET enabled = ? WHERE rollout_id = ?",
                    (1 if enabled else 0, rollout_id),
                )
            connection.execute(
                "UPDATE rollouts SET updated_at = ? WHERE rollout_id = ?",
                (now, rollout_id),
            )
            return self._get_rollout(connection, rollout_id)

    def delete_rollout(self, rollout_id: str) -> None:
        with self._connection() as connection:
            self._require_rollout(connection, rollout_id)
            connection.execute(
                "DELETE FROM rollouts WHERE rollout_id = ?", (rollout_id,)
            )

    @staticmethod
    def _gray_selected(rollout_id: str, device_id: str, percent: int) -> bool:
        if percent >= 100:
            return True
        if percent <= 0:
            return False
        digest = hashlib.sha256(f"{rollout_id}:{device_id}".encode()).hexdigest()
        return int(digest[:8], 16) % 100 < percent

    def select_ota_target(
        self,
        device_id: str,
        product_id: str,
        hw_version: str,
        current_fw_version: str,
    ) -> tuple[FirmwareResponse | None, str]:
        """Resolve the firmware a device should upgrade to (with a reason code)."""
        with self._connection() as connection:
            rows = connection.execute(
                """
                SELECT * FROM rollouts
                WHERE enabled = 1 AND product_id = ? AND hw_version = ?
                ORDER BY updated_at DESC, rollout_id DESC
                """,
                (product_id, hw_version),
            ).fetchall()
            candidates = [
                row
                for row in rows
                if row["from_fw_version"] is None
                or str(row["from_fw_version"]) == current_fw_version
            ]
            if not candidates:
                return None, "no_rollout"
            rollout = candidates[0]
            target_version = str(rollout["target_fw_version"])
            if target_version == current_fw_version:
                return None, "up_to_date"
            if not self._gray_selected(
                str(rollout["rollout_id"]), device_id, int(rollout["percent"])
            ):
                return None, "not_selected"
            firmware_row = connection.execute(
                """
                SELECT * FROM firmware
                WHERE product_id = ? AND hw_version = ? AND fw_version = ?
                """,
                (product_id, hw_version, target_version),
            ).fetchone()
            if firmware_row is None:
                return None, "firmware_missing"
            return self._firmware_from_row(firmware_row), "match"

    def begin_ota_dispatch(
        self,
        device_id: str,
        firmware: FirmwareResponse,
        message_id: str,
    ) -> tuple[OtaUpdateRecord, bool]:
        """Idempotently create a pending OTA record; returns (record, created)."""
        now = _utc_now().isoformat()
        with self._connection() as connection:
            self._require_device(connection, device_id)
            existing = connection.execute(
                "SELECT * FROM ota_updates WHERE device_id = ? AND firmware_id = ?",
                (device_id, firmware.firmware_id),
            ).fetchone()
            if existing is not None and str(existing["status"]) != "failed":
                return self._ota_from_row(existing), False
            if existing is None:
                cursor = connection.execute(
                    """
                    INSERT INTO ota_updates(
                        device_id, firmware_id, product_id, hw_version,
                        target_fw_version, status, message_id, created_at, updated_at
                    ) VALUES (?, ?, ?, ?, ?, 'pending', ?, ?, ?)
                    """,
                    (
                        device_id, firmware.firmware_id, firmware.product_id,
                        firmware.hw_version, firmware.fw_version, message_id, now, now,
                    ),
                )
                record = self._get_ota(connection, int(cursor.lastrowid))
            else:
                connection.execute(
                    """
                    UPDATE ota_updates
                    SET status = 'pending', message_id = ?, updated_at = ?
                    WHERE update_id = ?
                    """,
                    (message_id, now, int(existing["update_id"])),
                )
                record = self._get_ota(connection, int(existing["update_id"]))
            return record, True

    def record_ota_result(
        self,
        device_id: str,
        status: str,
        fw_version: str | None = None,
        message_id: str | None = None,
    ) -> OtaUpdateRecord:
        now = _utc_now().isoformat()
        with self._connection() as connection:
            self._require_device(connection, device_id)
            if fw_version is not None:
                row = connection.execute(
                    """
                    SELECT * FROM ota_updates
                    WHERE device_id = ? AND target_fw_version = ?
                    ORDER BY update_id DESC LIMIT 1
                    """,
                    (device_id, fw_version),
                ).fetchone()
            else:
                row = connection.execute(
                    """
                    SELECT * FROM ota_updates WHERE device_id = ?
                    ORDER BY update_id DESC LIMIT 1
                    """,
                    (device_id,),
                ).fetchone()
            if row is None:
                raise OtaUpdateNotFoundError(device_id)
            connection.execute(
                "UPDATE ota_updates SET status = ?, message_id = COALESCE(?, message_id), "
                "updated_at = ? WHERE update_id = ?",
                (status, message_id, now, int(row["update_id"])),
            )
            return self._get_ota(connection, int(row["update_id"]))

    def list_ota_updates(self, device_id: str) -> list[OtaUpdateRecord]:
        with self._connection() as connection:
            self._require_device(connection, device_id)
            rows = connection.execute(
                "SELECT * FROM ota_updates WHERE device_id = ? ORDER BY update_id DESC",
                (device_id,),
            ).fetchall()
            return [self._ota_from_row(row) for row in rows]

    # --- helpers -----------------------------------------------------------

    def _get_device(self, connection: sqlite3.Connection, device_id: str) -> DeviceResponse:
        row = connection.execute(
            "SELECT * FROM devices WHERE device_id = ?", (device_id,)
        ).fetchone()
        if row is None:
            raise DeviceNotFoundError(device_id)
        return self._device_from_row(row)

    def _require_device(self, connection: sqlite3.Connection, device_id: str) -> None:
        row = connection.execute(
            "SELECT 1 FROM devices WHERE device_id = ?", (device_id,)
        ).fetchone()
        if row is None:
            raise DeviceNotFoundError(device_id)

    def _get_shadow(self, connection: sqlite3.Connection, device_id: str) -> ShadowResponse:
        row = connection.execute(
            """
            SELECT device_shadows.*, devices.capabilities_json
            FROM device_shadows
            JOIN devices ON devices.device_id = device_shadows.device_id
            WHERE device_shadows.device_id = ?
            """,
            (device_id,),
        ).fetchone()
        if row is None:
            raise DeviceNotFoundError(device_id)
        return self._shadow_from_row(row)

    def _get_room(self, connection: sqlite3.Connection, room_id: str) -> RoomResponse:
        row = connection.execute(
            "SELECT * FROM rooms WHERE room_id = ?", (room_id,)
        ).fetchone()
        if row is None:
            raise RoomNotFoundError(room_id)
        device_rows = connection.execute(
            "SELECT device_id FROM devices WHERE room_id = ? ORDER BY device_id",
            (room_id,),
        ).fetchall()
        return RoomResponse(
            room_id=str(row["room_id"]),
            owner_id=str(row["owner_id"]),
            name=str(row["name"]),
            device_ids=[str(item["device_id"]) for item in device_rows],
            created_at=datetime.fromisoformat(str(row["created_at"])),
            updated_at=datetime.fromisoformat(str(row["updated_at"])),
        )

    def _require_room(self, connection: sqlite3.Connection, room_id: str) -> None:
        row = connection.execute(
            "SELECT 1 FROM rooms WHERE room_id = ?", (room_id,)
        ).fetchone()
        if row is None:
            raise RoomNotFoundError(room_id)

    def _group_member_ids(self, connection: sqlite3.Connection, group_id: str) -> list[str]:
        rows = connection.execute(
            "SELECT device_id FROM group_members WHERE group_id = ? ORDER BY device_id",
            (group_id,),
        ).fetchall()
        return [str(row["device_id"]) for row in rows]

    def _get_group(self, connection: sqlite3.Connection, group_id: str) -> GroupResponse:
        row = connection.execute(
            "SELECT * FROM device_groups WHERE group_id = ?", (group_id,)
        ).fetchone()
        if row is None:
            raise GroupNotFoundError(group_id)
        return GroupResponse(
            group_id=str(row["group_id"]),
            owner_id=str(row["owner_id"]),
            name=str(row["name"]),
            device_ids=self._group_member_ids(connection, group_id),
            created_at=datetime.fromisoformat(str(row["created_at"])),
            updated_at=datetime.fromisoformat(str(row["updated_at"])),
        )

    def _require_group(self, connection: sqlite3.Connection, group_id: str) -> None:
        row = connection.execute(
            "SELECT 1 FROM device_groups WHERE group_id = ?", (group_id,)
        ).fetchone()
        if row is None:
            raise GroupNotFoundError(group_id)

    def _scene_actions(
        self, connection: sqlite3.Connection, scene_id: str
    ) -> list[SceneAction]:
        rows = connection.execute(
            "SELECT device_id, state_json FROM scene_actions WHERE scene_id = ? "
            "ORDER BY device_id",
            (scene_id,),
        ).fetchall()
        return [
            SceneAction(
                device_id=str(row["device_id"]),
                state=_load_object(str(row["state_json"])),
            )
            for row in rows
        ]

    def _replace_scene_actions(
        self,
        connection: sqlite3.Connection,
        scene_id: str,
        actions: list[SceneAction],
    ) -> None:
        connection.execute("DELETE FROM scene_actions WHERE scene_id = ?", (scene_id,))
        for action in actions:
            self._require_device(connection, action.device_id)
            connection.execute(
                """
                INSERT INTO scene_actions(scene_id, device_id, state_json)
                VALUES (?, ?, ?)
                """,
                (scene_id, action.device_id, _dump_object(action.state)),
            )

    def _get_scene(self, connection: sqlite3.Connection, scene_id: str) -> SceneResponse:
        row = connection.execute(
            "SELECT * FROM scenes WHERE scene_id = ?", (scene_id,)
        ).fetchone()
        if row is None:
            raise SceneNotFoundError(scene_id)
        return SceneResponse(
            scene_id=str(row["scene_id"]),
            owner_id=str(row["owner_id"]),
            name=str(row["name"]),
            actions=self._scene_actions(connection, scene_id),
            created_at=datetime.fromisoformat(str(row["created_at"])),
            updated_at=datetime.fromisoformat(str(row["updated_at"])),
        )

    def _require_scene(self, connection: sqlite3.Connection, scene_id: str) -> None:
        row = connection.execute(
            "SELECT 1 FROM scenes WHERE scene_id = ?", (scene_id,)
        ).fetchone()
        if row is None:
            raise SceneNotFoundError(scene_id)

    def _get_automation(
        self, connection: sqlite3.Connection, automation_id: str
    ) -> AutomationResponse:
        row = connection.execute(
            "SELECT * FROM automations WHERE automation_id = ?", (automation_id,)
        ).fetchone()
        if row is None:
            raise AutomationNotFoundError(automation_id)
        return AutomationResponse(
            automation_id=str(row["automation_id"]),
            owner_id=str(row["owner_id"]),
            name=str(row["name"]),
            enabled=bool(row["enabled"]),
            trigger=AutomationTrigger.model_validate_json(str(row["trigger_json"])),
            action=AutomationAction.model_validate_json(str(row["action_json"])),
            created_at=datetime.fromisoformat(str(row["created_at"])),
            updated_at=datetime.fromisoformat(str(row["updated_at"])),
        )

    def _require_automation(
        self, connection: sqlite3.Connection, automation_id: str
    ) -> None:
        row = connection.execute(
            "SELECT 1 FROM automations WHERE automation_id = ?", (automation_id,)
        ).fetchone()
        if row is None:
            raise AutomationNotFoundError(automation_id)

    def _get_firmware(
        self, connection: sqlite3.Connection, firmware_id: str
    ) -> FirmwareResponse:
        row = connection.execute(
            "SELECT * FROM firmware WHERE firmware_id = ?", (firmware_id,)
        ).fetchone()
        if row is None:
            raise FirmwareNotFoundError(firmware_id)
        return self._firmware_from_row(row)

    def _require_firmware(
        self, connection: sqlite3.Connection, firmware_id: str
    ) -> None:
        row = connection.execute(
            "SELECT 1 FROM firmware WHERE firmware_id = ?", (firmware_id,)
        ).fetchone()
        if row is None:
            raise FirmwareNotFoundError(firmware_id)

    def _get_rollout(
        self, connection: sqlite3.Connection, rollout_id: str
    ) -> RolloutResponse:
        row = connection.execute(
            "SELECT * FROM rollouts WHERE rollout_id = ?", (rollout_id,)
        ).fetchone()
        if row is None:
            raise RolloutNotFoundError(rollout_id)
        return self._rollout_from_row(row)

    def _require_rollout(
        self, connection: sqlite3.Connection, rollout_id: str
    ) -> None:
        row = connection.execute(
            "SELECT 1 FROM rollouts WHERE rollout_id = ?", (rollout_id,)
        ).fetchone()
        if row is None:
            raise RolloutNotFoundError(rollout_id)

    def _get_ota(
        self, connection: sqlite3.Connection, update_id: int
    ) -> OtaUpdateRecord:
        row = connection.execute(
            "SELECT * FROM ota_updates WHERE update_id = ?", (update_id,)
        ).fetchone()
        if row is None:
            raise OtaUpdateNotFoundError(str(update_id))
        return self._ota_from_row(row)

    @staticmethod
    def _firmware_from_row(row: sqlite3.Row) -> FirmwareResponse:
        sign_value = row["sign"]
        notes_value = row["notes"]
        return FirmwareResponse(
            firmware_id=str(row["firmware_id"]),
            product_id=str(row["product_id"]),
            hw_version=str(row["hw_version"]),
            fw_version=str(row["fw_version"]),
            url=str(row["url"]),
            sha256=str(row["sha256"]),
            sign=None if sign_value is None else str(sign_value),
            notes=None if notes_value is None else str(notes_value),
            created_at=datetime.fromisoformat(str(row["created_at"])),
            updated_at=datetime.fromisoformat(str(row["updated_at"])),
        )

    @staticmethod
    def _rollout_from_row(row: sqlite3.Row) -> RolloutResponse:
        from_value = row["from_fw_version"]
        return RolloutResponse(
            rollout_id=str(row["rollout_id"]),
            product_id=str(row["product_id"]),
            hw_version=str(row["hw_version"]),
            target_fw_version=str(row["target_fw_version"]),
            from_fw_version=None if from_value is None else str(from_value),
            percent=int(row["percent"]),
            enabled=bool(row["enabled"]),
            created_at=datetime.fromisoformat(str(row["created_at"])),
            updated_at=datetime.fromisoformat(str(row["updated_at"])),
        )

    @staticmethod
    def _ota_from_row(row: sqlite3.Row) -> OtaUpdateRecord:
        firmware_value = row["firmware_id"]
        message_value = row["message_id"]
        return OtaUpdateRecord(
            update_id=int(row["update_id"]),
            device_id=str(row["device_id"]),
            firmware_id=None if firmware_value is None else str(firmware_value),
            product_id=str(row["product_id"]),
            hw_version=str(row["hw_version"]),
            target_fw_version=str(row["target_fw_version"]),
            status=str(row["status"]),
            message_id=None if message_value is None else str(message_value),
            created_at=datetime.fromisoformat(str(row["created_at"])),
            updated_at=datetime.fromisoformat(str(row["updated_at"])),
        )

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

    def _get_event(
        self, connection: sqlite3.Connection, event_id: int
    ) -> DeviceEventResponse:
        row = connection.execute(
            "SELECT * FROM device_events WHERE event_id = ?", (event_id,)
        ).fetchone()
        return self._event_from_row(row)

    @staticmethod
    def _event_from_row(row: sqlite3.Row) -> DeviceEventResponse:
        message_value = row["message_id"]
        return DeviceEventResponse(
            event_id=int(row["event_id"]),
            device_id=str(row["device_id"]),
            event=str(row["event"]),
            payload=_load_object(str(row["payload_json"])),
            message_id=None if message_value is None else str(message_value),
            created_at=datetime.fromisoformat(str(row["created_at"])),
        )

    @staticmethod
    def _account_from_row(row: sqlite3.Row) -> AccountResponse:
        return AccountResponse(
            user_id=str(row["user_id"]),
            email=str(row["email"]),
            created_at=datetime.fromisoformat(str(row["created_at"])),
        )

    @staticmethod
    def _device_from_row(row: sqlite3.Row) -> DeviceResponse:
        owner_value = row["owner_id"]
        name_value = row["name"]
        room_value = row["room_id"]
        caps_raw = row["capabilities_json"]
        return DeviceResponse(
            device_id=str(row["device_id"]),
            owner_id=None if owner_value is None else str(owner_value),
            type=str(row["type"]),
            name=None if name_value is None else str(name_value),
            room_id=None if room_value is None else str(room_value),
            metadata=_load_object(str(row["metadata_json"])),
            capabilities=_load_string_list(None if caps_raw is None else str(caps_raw)),
            created_at=datetime.fromisoformat(str(row["created_at"])),
            updated_at=datetime.fromisoformat(str(row["updated_at"])),
        )

    @staticmethod
    def _shadow_from_row(row: sqlite3.Row) -> ShadowResponse:
        offline_value = row["offline_reason"]
        last_seen_value = row["last_seen_at"]
        return ShadowResponse(
            device_id=str(row["device_id"]),
            capabilities=_load_string_list(str(row["capabilities_json"])),
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
