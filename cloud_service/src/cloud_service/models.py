from __future__ import annotations

from datetime import datetime
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, JsonValue, model_validator

DEVICE_ID_PATTERN = r"^[A-Za-z0-9._:-]{1,96}$"
USER_ID_PATTERN = r"^[A-Za-z0-9._:@-]{1,96}$"
TYPE_PATTERN = r"^[a-z][a-z0-9_]{0,47}$"


class ApiModel(BaseModel):
    model_config = ConfigDict(extra="forbid")


class RegisterDeviceRequest(ApiModel):
    device_id: str = Field(pattern=DEVICE_ID_PATTERN)
    type: str = Field(pattern=TYPE_PATTERN)
    name: str | None = Field(default=None, max_length=96)
    metadata: dict[str, JsonValue] = Field(default_factory=dict)


class ClaimDeviceRequest(ApiModel):
    user_id: str = Field(pattern=USER_ID_PATTERN)


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
    created_at: datetime
    updated_at: datetime


class ShadowResponse(ApiModel):
    device_id: str
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
