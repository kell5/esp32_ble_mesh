from __future__ import annotations

from datetime import datetime
from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, JsonValue, model_validator

DEVICE_ID_PATTERN = r"^[A-Za-z0-9._:-]{1,96}$"
USER_ID_PATTERN = r"^[A-Za-z0-9._:@-]{1,96}$"
TYPE_PATTERN = r"^[a-z][a-z0-9_]{0,47}$"
EMAIL_PATTERN = r"^[^@\s]+@[^@\s]+\.[^@\s]+$"
CAPABILITY_PATTERN = r"^[a-z][a-z0-9_.-]{0,63}$"
PRODUCT_ID_PATTERN = r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"
HW_VERSION_PATTERN = r"^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$"
FW_VERSION_PATTERN = r"^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$"
SHA256_PATTERN = r"^[A-Fa-f0-9]{64}$"

CapabilityName = Annotated[str, Field(pattern=CAPABILITY_PATTERN)]

OtaReason = Literal[
    "dispatched",
    "already_dispatched",
    "no_rollout",
    "up_to_date",
    "not_selected",
    "firmware_missing",
]
OtaStatus = Literal["pending", "downloading", "success", "failed"]


class ApiModel(BaseModel):
    model_config = ConfigDict(extra="forbid")


class RegisterAccountRequest(ApiModel):
    email: str = Field(pattern=EMAIL_PATTERN, max_length=128)
    password: str = Field(min_length=6, max_length=128)


class LoginRequest(ApiModel):
    email: str = Field(pattern=EMAIL_PATTERN, max_length=128)
    password: str = Field(min_length=6, max_length=128)


class AccountResponse(ApiModel):
    user_id: str
    email: str
    created_at: datetime


class AuthResponse(ApiModel):
    user_id: str
    email: str
    token: str


class RegisterDeviceRequest(ApiModel):
    device_id: str = Field(pattern=DEVICE_ID_PATTERN)
    type: str = Field(pattern=TYPE_PATTERN)
    name: str | None = Field(default=None, max_length=96)
    metadata: dict[str, JsonValue] = Field(default_factory=dict)
    capabilities: list[CapabilityName] = Field(default_factory=list, max_length=64)


class ClaimDeviceRequest(ApiModel):
    user_id: str = Field(pattern=USER_ID_PATTERN)


class ClaimMyDeviceRequest(ApiModel):
    """Body for /me/devices/{id}/claim; `force` transfers ownership after the
    device was physically re-provisioned by the new owner."""

    force: bool = False


class ShadowPatchRequest(ApiModel):
    state: dict[str, JsonValue]
    message_id: str | None = Field(default=None, min_length=1, max_length=128)


class OfflineRequest(ApiModel):
    reason: str = Field(min_length=1, max_length=96)


class DeviceResponse(ApiModel):
    device_id: str
    owner_id: str | None
    type: str
    name: str | None
    room_id: str | None
    metadata: dict[str, JsonValue]
    capabilities: list[str] = Field(default_factory=list)
    created_at: datetime
    updated_at: datetime


class ShadowResponse(ApiModel):
    device_id: str
    capabilities: list[str] = Field(default_factory=list)
    desired: dict[str, JsonValue]
    reported: dict[str, JsonValue]
    desired_version: int
    reported_version: int
    version: int
    offline_reason: str | None
    last_seen_at: datetime | None
    updated_at: datetime


class DesiredUpdateResponse(ApiModel):
    shadow: ShadowResponse
    changed: bool
    published: bool


class HealthResponse(ApiModel):
    status: str
    mqtt_enabled: bool
    mqtt_connected: bool


class RoomRequest(ApiModel):
    name: str = Field(min_length=1, max_length=96)


class RoomUpdateRequest(ApiModel):
    name: str = Field(min_length=1, max_length=96)


class RoomResponse(ApiModel):
    room_id: str
    owner_id: str
    name: str
    device_ids: list[str]
    created_at: datetime
    updated_at: datetime


class GroupRequest(ApiModel):
    name: str = Field(min_length=1, max_length=96)


class GroupUpdateRequest(ApiModel):
    name: str = Field(min_length=1, max_length=96)


class GroupResponse(ApiModel):
    group_id: str
    owner_id: str
    name: str
    device_ids: list[str]
    created_at: datetime
    updated_at: datetime


class SceneAction(ApiModel):
    device_id: str = Field(pattern=DEVICE_ID_PATTERN)
    state: dict[str, JsonValue]


class SceneRequest(ApiModel):
    name: str = Field(min_length=1, max_length=96)
    actions: list[SceneAction] = Field(default_factory=list)


class SceneUpdateRequest(ApiModel):
    name: str | None = Field(default=None, min_length=1, max_length=96)
    actions: list[SceneAction] | None = None


class SceneResponse(ApiModel):
    scene_id: str
    owner_id: str
    name: str
    actions: list[SceneAction]
    created_at: datetime
    updated_at: datetime


class DeviceEventResponse(ApiModel):
    event_id: int
    device_id: str
    event: str
    payload: dict[str, JsonValue]
    message_id: str | None
    created_at: datetime


class CommandResult(ApiModel):
    device_id: str
    changed: bool
    published: bool
    found: bool


class ApplyResponse(ApiModel):
    results: list[CommandResult]


class GroupCommandRequest(ApiModel):
    state: dict[str, JsonValue]
    message_id: str | None = Field(default=None, min_length=1, max_length=128)


class AutomationTrigger(ApiModel):
    device_id: str = Field(pattern=DEVICE_ID_PATTERN)
    field: str = Field(min_length=1, max_length=64)
    equals: JsonValue


class AutomationAction(ApiModel):
    type: Literal["device", "group", "scene"]
    device_id: str | None = Field(default=None, pattern=DEVICE_ID_PATTERN)
    group_id: str | None = None
    scene_id: str | None = None
    state: dict[str, JsonValue] | None = None

    @model_validator(mode="after")
    def _check_target(self) -> AutomationAction:
        if self.type == "device":
            if self.device_id is None or self.state is None:
                raise ValueError("device action requires device_id and state")
        elif self.type == "group":
            if self.group_id is None or self.state is None:
                raise ValueError("group action requires group_id and state")
        elif self.type == "scene":
            if self.scene_id is None:
                raise ValueError("scene action requires scene_id")
        return self


class AutomationRequest(ApiModel):
    name: str = Field(min_length=1, max_length=96)
    enabled: bool = True
    trigger: AutomationTrigger
    action: AutomationAction


class AutomationUpdateRequest(ApiModel):
    name: str | None = Field(default=None, min_length=1, max_length=96)
    enabled: bool | None = None
    trigger: AutomationTrigger | None = None
    action: AutomationAction | None = None


class AutomationResponse(ApiModel):
    automation_id: str
    owner_id: str
    name: str
    enabled: bool
    trigger: AutomationTrigger
    action: AutomationAction
    created_at: datetime
    updated_at: datetime


class FirmwareRequest(ApiModel):
    product_id: str = Field(pattern=PRODUCT_ID_PATTERN)
    hw_version: str = Field(pattern=HW_VERSION_PATTERN)
    fw_version: str = Field(pattern=FW_VERSION_PATTERN)
    url: str = Field(min_length=1, max_length=1024)
    sha256: str = Field(pattern=SHA256_PATTERN)
    sign: str | None = Field(default=None, max_length=1024)
    notes: str | None = Field(default=None, max_length=512)

    @model_validator(mode="after")
    def _check_url(self) -> FirmwareRequest:
        if not (self.url.startswith("https://") or self.url.startswith("http://")):
            raise ValueError("url must be an http(s) URL")
        return self


class FirmwareResponse(ApiModel):
    firmware_id: str
    product_id: str
    hw_version: str
    fw_version: str
    url: str
    sha256: str
    sign: str | None
    notes: str | None
    created_at: datetime
    updated_at: datetime


class RolloutRequest(ApiModel):
    product_id: str = Field(pattern=PRODUCT_ID_PATTERN)
    hw_version: str = Field(pattern=HW_VERSION_PATTERN)
    target_fw_version: str = Field(pattern=FW_VERSION_PATTERN)
    from_fw_version: str | None = Field(default=None, pattern=FW_VERSION_PATTERN)
    percent: int = Field(default=100, ge=0, le=100)
    enabled: bool = True


class RolloutUpdateRequest(ApiModel):
    target_fw_version: str | None = Field(default=None, pattern=FW_VERSION_PATTERN)
    from_fw_version: str | None = Field(default=None, pattern=FW_VERSION_PATTERN)
    percent: int | None = Field(default=None, ge=0, le=100)
    enabled: bool | None = None


class RolloutResponse(ApiModel):
    rollout_id: str
    product_id: str
    hw_version: str
    target_fw_version: str
    from_fw_version: str | None
    percent: int
    enabled: bool
    created_at: datetime
    updated_at: datetime


class OtaUpdateRecord(ApiModel):
    update_id: int
    device_id: str
    firmware_id: str | None
    product_id: str
    hw_version: str
    target_fw_version: str
    status: OtaStatus
    message_id: str | None
    created_at: datetime
    updated_at: datetime


class OtaCheckRequest(ApiModel):
    product_id: str = Field(pattern=PRODUCT_ID_PATTERN)
    hw_version: str = Field(pattern=HW_VERSION_PATTERN)
    fw_version: str = Field(pattern=FW_VERSION_PATTERN)


class OtaCheckResponse(ApiModel):
    dispatched: bool
    published: bool
    reason: OtaReason
    target: FirmwareResponse | None
    update: OtaUpdateRecord | None


class OtaProgressRequest(ApiModel):
    status: OtaStatus
    fw_version: str | None = Field(default=None, pattern=FW_VERSION_PATTERN)
    message_id: str | None = Field(default=None, min_length=1, max_length=128)
