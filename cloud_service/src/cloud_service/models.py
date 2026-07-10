from datetime import datetime

from pydantic import BaseModel, ConfigDict, Field, JsonValue

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
