from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager, suppress
from datetime import datetime, timedelta, timezone
from typing import Annotated

from fastapi import Depends, FastAPI, Header, HTTPException, Query, Response, status

from .automation import AutomationEngine
from .config import Settings
from .models import (
    AccountResponse,
    ApplyResponse,
    AuthResponse,
    AutomationRequest,
    AutomationResponse,
    AutomationUpdateRequest,
    ClaimDeviceRequest,
    ClaimMyDeviceRequest,
    DesiredUpdateResponse,
    DeviceEventResponse,
    DeviceResponse,
    GroupCommandRequest,
    GroupRequest,
    GroupResponse,
    GroupUpdateRequest,
    FirmwareRequest,
    FirmwareResponse,
    HealthResponse,
    LoginRequest,
    OfflineRequest,
    OtaCheckRequest,
    OtaCheckResponse,
    OtaProgressRequest,
    OtaUpdateRecord,
    RegisterAccountRequest,
    RegisterDeviceRequest,
    RolloutRequest,
    RolloutResponse,
    RolloutUpdateRequest,
    RoomRequest,
    RoomResponse,
    RoomUpdateRequest,
    SceneRequest,
    SceneResponse,
    SceneUpdateRequest,
    ShadowPatchRequest,
    ShadowResponse,
)
from .mqtt_bridge import MqttBridge
from .ota import OtaEngine
from .security import Principal
from .storage import (
    AutomationNotFoundError,
    DeviceAlreadyClaimedError,
    DeviceNotFoundError,
    DeviceNotOwnedError,
    DeviceStore,
    EmailAlreadyExistsError,
    FirmwareNotFoundError,
    GroupNotFoundError,
    InvalidCredentialsError,
    OtaUpdateNotFoundError,
    RolloutNotFoundError,
    RoomNotFoundError,
    SceneNotFoundError,
)


async def _stale_sweeper(store: DeviceStore, settings: Settings) -> None:
    interval = max(10, settings.stale_after_seconds // 3)
    while True:
        cutoff = datetime.now(timezone.utc) - timedelta(seconds=settings.stale_after_seconds)
        await asyncio.to_thread(store.mark_stale_devices, cutoff)
        await asyncio.sleep(interval)


def create_app(settings: Settings | None = None) -> FastAPI:
    active_settings = settings or Settings.from_env()
    store = DeviceStore(active_settings.database_path)
    bridge = MqttBridge(store, active_settings)
    engine = AutomationEngine(store, bridge.publish_desired)
    bridge.set_reported_handler(engine.on_reported)
    ota_engine = OtaEngine(store, bridge.publish_ota)
    bridge.set_version_handler(ota_engine.on_version_report)

    @asynccontextmanager
    async def lifespan(_app: FastAPI):
        sweeper: asyncio.Task[None] | None = None
        if active_settings.mqtt_enabled:
            bridge.start()
            sweeper = asyncio.create_task(_stale_sweeper(store, active_settings))
        try:
            yield
        finally:
            if sweeper is not None:
                sweeper.cancel()
                with suppress(asyncio.CancelledError):
                    await sweeper
            bridge.stop()

    application = FastAPI(
        title="Mesh Smart Home Cloud API",
        version="0.1.0",
        lifespan=lifespan,
    )

    def authorize(
        x_cloud_token: Annotated[str | None, Header()] = None,
    ) -> None:
        expected = active_settings.api_token
        if expected is not None and x_cloud_token != expected:
            raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="invalid token")

    authorization = Depends(authorize)

    def get_principal(
        authorization_header: Annotated[str | None, Header(alias="Authorization")] = None,
        x_cloud_token: Annotated[str | None, Header()] = None,
    ) -> Principal:
        if authorization_header and authorization_header.lower().startswith("bearer "):
            token = authorization_header[7:].strip()
            user_id = store.resolve_token(token)
            if user_id is None:
                raise HTTPException(
                    status_code=status.HTTP_401_UNAUTHORIZED, detail="invalid bearer token"
                )
            return Principal(is_admin=False, user_id=user_id)
        expected = active_settings.api_token
        if expected is None or x_cloud_token == expected:
            return Principal(is_admin=True)
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="invalid token")

    def require_account(principal: Principal) -> str:
        if principal.user_id is None:
            raise HTTPException(
                status_code=status.HTTP_403_FORBIDDEN, detail="requires user login"
            )
        return principal.user_id

    def enforce_user(principal: Principal, user_id: str) -> None:
        if not principal.is_admin and principal.user_id != user_id:
            raise HTTPException(status_code=status.HTTP_403_FORBIDDEN, detail="forbidden")

    def enforce_device(principal: Principal, device_id: str) -> DeviceResponse:
        try:
            device = store.get_device(device_id)
        except DeviceNotFoundError as error:
            raise missing_device(error) from error
        if not principal.is_admin and device.owner_id != principal.user_id:
            raise HTTPException(status_code=status.HTTP_403_FORBIDDEN, detail="forbidden")
        return device

    def missing_device(error: DeviceNotFoundError) -> HTTPException:
        return HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail=f"device not found: {error}"
        )

    def not_found(kind: str, error: LookupError) -> HTTPException:
        return HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail=f"{kind} not found: {error}"
        )

    @application.get("/health", response_model=HealthResponse)
    def health() -> HealthResponse:
        return HealthResponse(
            status="ok",
            mqtt_enabled=active_settings.mqtt_enabled,
            mqtt_connected=bridge.connected,
        )

    # --- auth --------------------------------------------------------------

    @application.post(
        "/api/v1/auth/register",
        response_model=AuthResponse,
        status_code=status.HTTP_201_CREATED,
    )
    def register_account(request: RegisterAccountRequest) -> AuthResponse:
        try:
            account = store.create_account(request.email, request.password)
        except EmailAlreadyExistsError as error:
            raise HTTPException(
                status_code=status.HTTP_409_CONFLICT,
                detail="email already registered",
            ) from error
        token = store.issue_token(account.user_id)
        return AuthResponse(user_id=account.user_id, email=account.email, token=token)

    @application.post("/api/v1/auth/login", response_model=AuthResponse)
    def login(request: LoginRequest) -> AuthResponse:
        try:
            account = store.authenticate(request.email, request.password)
        except InvalidCredentialsError as error:
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail="invalid email or password",
            ) from error
        token = store.issue_token(account.user_id)
        return AuthResponse(user_id=account.user_id, email=account.email, token=token)

    @application.get("/api/v1/auth/me", response_model=AccountResponse)
    def whoami(
        principal: Principal = Depends(get_principal),
    ) -> AccountResponse:
        user_id = require_account(principal)
        return store.get_account(user_id)

    @application.post("/api/v1/auth/logout", status_code=status.HTTP_204_NO_CONTENT)
    def logout(
        authorization_header: Annotated[str | None, Header(alias="Authorization")] = None,
    ) -> Response:
        if authorization_header and authorization_header.lower().startswith("bearer "):
            store.revoke_token(authorization_header[7:].strip())
        return Response(status_code=status.HTTP_204_NO_CONTENT)

    # --- current-user device plane ----------------------------------------

    @application.get(
        "/api/v1/me/devices",
        response_model=list[DeviceResponse],
    )
    def list_my_devices(
        principal: Principal = Depends(get_principal),
    ) -> list[DeviceResponse]:
        return store.list_devices(require_account(principal))

    @application.post(
        "/api/v1/me/devices/{device_id}/claim",
        response_model=DeviceResponse,
    )
    def claim_my_device(
        device_id: str,
        request: ClaimMyDeviceRequest | None = None,
        principal: Principal = Depends(get_principal),
    ) -> DeviceResponse:
        user_id = require_account(principal)
        force = request.force if request is not None else False
        try:
            return store.claim_device(device_id, user_id, force=force)
        except DeviceNotFoundError as error:
            raise missing_device(error) from error
        except DeviceAlreadyClaimedError as error:
            raise HTTPException(
                status_code=status.HTTP_409_CONFLICT,
                detail="device already claimed by another account",
            ) from error

    @application.delete(
        "/api/v1/me/devices/{device_id}",
        status_code=status.HTTP_204_NO_CONTENT,
    )
    def unclaim_my_device(
        device_id: str,
        principal: Principal = Depends(get_principal),
    ) -> Response:
        user_id = require_account(principal)
        try:
            store.unclaim_device(device_id, user_id)
        except DeviceNotFoundError as error:
            raise missing_device(error) from error
        except DeviceNotOwnedError as error:
            raise HTTPException(
                status_code=status.HTTP_403_FORBIDDEN,
                detail="device not owned by this account",
            ) from error
        return Response(status_code=status.HTTP_204_NO_CONTENT)

    @application.get(
        "/api/v1/me/events",
        response_model=list[DeviceEventResponse],
    )
    def list_my_events(
        principal: Principal = Depends(get_principal),
        limit: int = Query(default=50, ge=1, le=200),
        before_id: int | None = Query(default=None, ge=1),
    ) -> list[DeviceEventResponse]:
        return store.list_owner_events(require_account(principal), limit, before_id)

    @application.get(
        "/api/v1/devices/{device_id}/events",
        response_model=list[DeviceEventResponse],
    )
    def list_device_events(
        device_id: str,
        principal: Principal = Depends(get_principal),
        limit: int = Query(default=50, ge=1, le=200),
        before_id: int | None = Query(default=None, ge=1),
    ) -> list[DeviceEventResponse]:
        enforce_device(principal, device_id)
        return store.list_device_events(device_id, limit, before_id)

    # --- devices -----------------------------------------------------------

    @application.post(
        "/api/v1/devices",
        response_model=DeviceResponse,
        status_code=status.HTTP_201_CREATED,
        dependencies=[authorization],
    )
    def register_device(request: RegisterDeviceRequest) -> DeviceResponse:
        return store.register_device(
            request.device_id, request.type, request.name, request.metadata,
            capabilities=request.capabilities if "capabilities" in request.model_fields_set else None,
        )

    @application.get(
        "/api/v1/devices/{device_id}",
        response_model=DeviceResponse,
    )
    def get_device(
        device_id: str,
        principal: Principal = Depends(get_principal),
    ) -> DeviceResponse:
        return enforce_device(principal, device_id)

    @application.post(
        "/api/v1/devices/{device_id}/claim",
        response_model=DeviceResponse,
        dependencies=[authorization],
    )
    def claim_device(device_id: str, request: ClaimDeviceRequest) -> DeviceResponse:
        try:
            return store.claim_device(device_id, request.user_id)
        except DeviceNotFoundError as error:
            raise missing_device(error) from error
        except DeviceAlreadyClaimedError as error:
            raise HTTPException(
                status_code=status.HTTP_409_CONFLICT,
                detail=f"device already claimed: {error}",
            ) from error

    @application.get(
        "/api/v1/users/{user_id}/devices",
        response_model=list[DeviceResponse],
        dependencies=[authorization],
    )
    def list_devices(user_id: str) -> list[DeviceResponse]:
        return store.list_devices(user_id)

    @application.get(
        "/api/v1/devices/{device_id}/shadow",
        response_model=ShadowResponse,
    )
    def get_shadow(
        device_id: str,
        principal: Principal = Depends(get_principal),
    ) -> ShadowResponse:
        enforce_device(principal, device_id)
        return store.get_shadow(device_id)

    @application.patch(
        "/api/v1/devices/{device_id}/shadow/desired",
        response_model=DesiredUpdateResponse,
    )
    def update_desired(
        device_id: str,
        request: ShadowPatchRequest,
        principal: Principal = Depends(get_principal),
    ) -> DesiredUpdateResponse:
        enforce_device(principal, device_id)
        shadow, changed = store.update_desired(device_id, request.state, request.message_id)
        published = changed and bridge.publish_desired(device_id, request.state)
        return DesiredUpdateResponse(shadow=shadow, changed=changed, published=published)

    @application.post(
        "/api/v1/devices/{device_id}/offline",
        response_model=ShadowResponse,
        dependencies=[authorization],
    )
    def mark_offline(device_id: str, request: OfflineRequest) -> ShadowResponse:
        try:
            return store.mark_offline(device_id, request.reason)
        except DeviceNotFoundError as error:
            raise missing_device(error) from error

    # --- rooms -------------------------------------------------------------

    @application.post(
        "/api/v1/users/{user_id}/rooms",
        response_model=RoomResponse,
        status_code=status.HTTP_201_CREATED,
        dependencies=[authorization],
    )
    def create_room(user_id: str, request: RoomRequest) -> RoomResponse:
        return store.create_room(user_id, request.name)

    @application.get(
        "/api/v1/users/{user_id}/rooms",
        response_model=list[RoomResponse],
        dependencies=[authorization],
    )
    def list_rooms(user_id: str) -> list[RoomResponse]:
        return store.list_rooms(user_id)

    @application.get(
        "/api/v1/rooms/{room_id}",
        response_model=RoomResponse,
        dependencies=[authorization],
    )
    def get_room(room_id: str) -> RoomResponse:
        try:
            return store.get_room(room_id)
        except RoomNotFoundError as error:
            raise not_found("room", error) from error

    @application.patch(
        "/api/v1/rooms/{room_id}",
        response_model=RoomResponse,
        dependencies=[authorization],
    )
    def rename_room(room_id: str, request: RoomUpdateRequest) -> RoomResponse:
        try:
            return store.rename_room(room_id, request.name)
        except RoomNotFoundError as error:
            raise not_found("room", error) from error

    @application.delete(
        "/api/v1/rooms/{room_id}",
        status_code=status.HTTP_204_NO_CONTENT,
        dependencies=[authorization],
    )
    def delete_room(room_id: str) -> Response:
        try:
            store.delete_room(room_id)
        except RoomNotFoundError as error:
            raise not_found("room", error) from error
        return Response(status_code=status.HTTP_204_NO_CONTENT)

    @application.put(
        "/api/v1/rooms/{room_id}/devices/{device_id}",
        response_model=RoomResponse,
        dependencies=[authorization],
    )
    def assign_device_to_room(room_id: str, device_id: str) -> RoomResponse:
        try:
            return store.assign_device_to_room(room_id, device_id)
        except RoomNotFoundError as error:
            raise not_found("room", error) from error
        except DeviceNotFoundError as error:
            raise missing_device(error) from error

    @application.delete(
        "/api/v1/rooms/{room_id}/devices/{device_id}",
        response_model=RoomResponse,
        dependencies=[authorization],
    )
    def remove_device_from_room(room_id: str, device_id: str) -> RoomResponse:
        try:
            return store.remove_device_from_room(room_id, device_id)
        except RoomNotFoundError as error:
            raise not_found("room", error) from error
        except DeviceNotFoundError as error:
            raise missing_device(error) from error

    # --- groups ------------------------------------------------------------

    @application.post(
        "/api/v1/users/{user_id}/groups",
        response_model=GroupResponse,
        status_code=status.HTTP_201_CREATED,
        dependencies=[authorization],
    )
    def create_group(user_id: str, request: GroupRequest) -> GroupResponse:
        return store.create_group(user_id, request.name)

    @application.get(
        "/api/v1/users/{user_id}/groups",
        response_model=list[GroupResponse],
        dependencies=[authorization],
    )
    def list_groups(user_id: str) -> list[GroupResponse]:
        return store.list_groups(user_id)

    @application.get(
        "/api/v1/groups/{group_id}",
        response_model=GroupResponse,
        dependencies=[authorization],
    )
    def get_group(group_id: str) -> GroupResponse:
        try:
            return store.get_group(group_id)
        except GroupNotFoundError as error:
            raise not_found("group", error) from error

    @application.patch(
        "/api/v1/groups/{group_id}",
        response_model=GroupResponse,
        dependencies=[authorization],
    )
    def rename_group(group_id: str, request: GroupUpdateRequest) -> GroupResponse:
        try:
            return store.rename_group(group_id, request.name)
        except GroupNotFoundError as error:
            raise not_found("group", error) from error

    @application.delete(
        "/api/v1/groups/{group_id}",
        status_code=status.HTTP_204_NO_CONTENT,
        dependencies=[authorization],
    )
    def delete_group(group_id: str) -> Response:
        try:
            store.delete_group(group_id)
        except GroupNotFoundError as error:
            raise not_found("group", error) from error
        return Response(status_code=status.HTTP_204_NO_CONTENT)

    @application.put(
        "/api/v1/groups/{group_id}/devices/{device_id}",
        response_model=GroupResponse,
        dependencies=[authorization],
    )
    def add_group_member(group_id: str, device_id: str) -> GroupResponse:
        try:
            return store.add_group_member(group_id, device_id)
        except GroupNotFoundError as error:
            raise not_found("group", error) from error
        except DeviceNotFoundError as error:
            raise missing_device(error) from error

    @application.delete(
        "/api/v1/groups/{group_id}/devices/{device_id}",
        response_model=GroupResponse,
        dependencies=[authorization],
    )
    def remove_group_member(group_id: str, device_id: str) -> GroupResponse:
        try:
            return store.remove_group_member(group_id, device_id)
        except GroupNotFoundError as error:
            raise not_found("group", error) from error

    @application.post(
        "/api/v1/groups/{group_id}/command",
        response_model=ApplyResponse,
        dependencies=[authorization],
    )
    def command_group(group_id: str, request: GroupCommandRequest) -> ApplyResponse:
        try:
            results = engine.command_group(group_id, request.state, request.message_id)
        except GroupNotFoundError as error:
            raise not_found("group", error) from error
        return ApplyResponse(results=results)

    # --- scenes ------------------------------------------------------------

    @application.post(
        "/api/v1/users/{user_id}/scenes",
        response_model=SceneResponse,
        status_code=status.HTTP_201_CREATED,
        dependencies=[authorization],
    )
    def create_scene(user_id: str, request: SceneRequest) -> SceneResponse:
        try:
            return store.create_scene(user_id, request.name, request.actions)
        except DeviceNotFoundError as error:
            raise missing_device(error) from error

    @application.get(
        "/api/v1/users/{user_id}/scenes",
        response_model=list[SceneResponse],
        dependencies=[authorization],
    )
    def list_scenes(user_id: str) -> list[SceneResponse]:
        return store.list_scenes(user_id)

    @application.get(
        "/api/v1/scenes/{scene_id}",
        response_model=SceneResponse,
        dependencies=[authorization],
    )
    def get_scene(scene_id: str) -> SceneResponse:
        try:
            return store.get_scene(scene_id)
        except SceneNotFoundError as error:
            raise not_found("scene", error) from error

    @application.patch(
        "/api/v1/scenes/{scene_id}",
        response_model=SceneResponse,
        dependencies=[authorization],
    )
    def update_scene(scene_id: str, request: SceneUpdateRequest) -> SceneResponse:
        try:
            return store.update_scene(scene_id, request.name, request.actions)
        except SceneNotFoundError as error:
            raise not_found("scene", error) from error
        except DeviceNotFoundError as error:
            raise missing_device(error) from error

    @application.delete(
        "/api/v1/scenes/{scene_id}",
        status_code=status.HTTP_204_NO_CONTENT,
        dependencies=[authorization],
    )
    def delete_scene(scene_id: str) -> Response:
        try:
            store.delete_scene(scene_id)
        except SceneNotFoundError as error:
            raise not_found("scene", error) from error
        return Response(status_code=status.HTTP_204_NO_CONTENT)

    @application.post(
        "/api/v1/scenes/{scene_id}/activate",
        response_model=ApplyResponse,
        dependencies=[authorization],
    )
    def activate_scene(scene_id: str) -> ApplyResponse:
        try:
            results = engine.activate_scene(scene_id)
        except SceneNotFoundError as error:
            raise not_found("scene", error) from error
        return ApplyResponse(results=results)

    # --- automations -------------------------------------------------------

    @application.post(
        "/api/v1/users/{user_id}/automations",
        response_model=AutomationResponse,
        status_code=status.HTTP_201_CREATED,
        dependencies=[authorization],
    )
    def create_automation(user_id: str, request: AutomationRequest) -> AutomationResponse:
        return store.create_automation(
            user_id, request.name, request.enabled, request.trigger, request.action
        )

    @application.get(
        "/api/v1/users/{user_id}/automations",
        response_model=list[AutomationResponse],
        dependencies=[authorization],
    )
    def list_automations(user_id: str) -> list[AutomationResponse]:
        return store.list_automations(user_id)

    @application.get(
        "/api/v1/automations/{automation_id}",
        response_model=AutomationResponse,
        dependencies=[authorization],
    )
    def get_automation(automation_id: str) -> AutomationResponse:
        try:
            return store.get_automation(automation_id)
        except AutomationNotFoundError as error:
            raise not_found("automation", error) from error

    @application.patch(
        "/api/v1/automations/{automation_id}",
        response_model=AutomationResponse,
        dependencies=[authorization],
    )
    def update_automation(
        automation_id: str, request: AutomationUpdateRequest
    ) -> AutomationResponse:
        try:
            return store.update_automation(
                automation_id,
                request.name,
                request.enabled,
                request.trigger,
                request.action,
            )
        except AutomationNotFoundError as error:
            raise not_found("automation", error) from error

    @application.delete(
        "/api/v1/automations/{automation_id}",
        status_code=status.HTTP_204_NO_CONTENT,
        dependencies=[authorization],
    )
    def delete_automation(automation_id: str) -> Response:
        try:
            store.delete_automation(automation_id)
        except AutomationNotFoundError as error:
            raise not_found("automation", error) from error
        return Response(status_code=status.HTTP_204_NO_CONTENT)

    # --- ota: firmware repository -----------------------------------------

    @application.post(
        "/api/v1/firmware",
        response_model=FirmwareResponse,
        status_code=status.HTTP_201_CREATED,
        dependencies=[authorization],
    )
    def register_firmware(request: FirmwareRequest) -> FirmwareResponse:
        return store.register_firmware(
            request.product_id,
            request.hw_version,
            request.fw_version,
            request.url,
            request.sha256,
            request.sign,
            request.notes,
        )

    @application.get(
        "/api/v1/firmware",
        response_model=list[FirmwareResponse],
        dependencies=[authorization],
    )
    def list_firmware(
        product_id: str | None = Query(default=None),
    ) -> list[FirmwareResponse]:
        return store.list_firmware(product_id)

    @application.get(
        "/api/v1/firmware/{firmware_id}",
        response_model=FirmwareResponse,
        dependencies=[authorization],
    )
    def get_firmware(firmware_id: str) -> FirmwareResponse:
        try:
            return store.get_firmware(firmware_id)
        except FirmwareNotFoundError as error:
            raise not_found("firmware", error) from error

    @application.delete(
        "/api/v1/firmware/{firmware_id}",
        status_code=status.HTTP_204_NO_CONTENT,
        dependencies=[authorization],
    )
    def delete_firmware(firmware_id: str) -> Response:
        try:
            store.delete_firmware(firmware_id)
        except FirmwareNotFoundError as error:
            raise not_found("firmware", error) from error
        return Response(status_code=status.HTTP_204_NO_CONTENT)

    # --- ota: rollouts -----------------------------------------------------

    @application.post(
        "/api/v1/rollouts",
        response_model=RolloutResponse,
        status_code=status.HTTP_201_CREATED,
        dependencies=[authorization],
    )
    def create_rollout(request: RolloutRequest) -> RolloutResponse:
        return store.create_rollout(
            request.product_id,
            request.hw_version,
            request.target_fw_version,
            request.from_fw_version,
            request.percent,
            request.enabled,
        )

    @application.get(
        "/api/v1/rollouts",
        response_model=list[RolloutResponse],
        dependencies=[authorization],
    )
    def list_rollouts() -> list[RolloutResponse]:
        return store.list_rollouts()

    @application.get(
        "/api/v1/rollouts/{rollout_id}",
        response_model=RolloutResponse,
        dependencies=[authorization],
    )
    def get_rollout(rollout_id: str) -> RolloutResponse:
        try:
            return store.get_rollout(rollout_id)
        except RolloutNotFoundError as error:
            raise not_found("rollout", error) from error

    @application.patch(
        "/api/v1/rollouts/{rollout_id}",
        response_model=RolloutResponse,
        dependencies=[authorization],
    )
    def update_rollout(
        rollout_id: str, request: RolloutUpdateRequest
    ) -> RolloutResponse:
        try:
            return store.update_rollout(
                rollout_id,
                request.target_fw_version,
                request.from_fw_version,
                request.percent,
                request.enabled,
            )
        except RolloutNotFoundError as error:
            raise not_found("rollout", error) from error

    @application.delete(
        "/api/v1/rollouts/{rollout_id}",
        status_code=status.HTTP_204_NO_CONTENT,
        dependencies=[authorization],
    )
    def delete_rollout(rollout_id: str) -> Response:
        try:
            store.delete_rollout(rollout_id)
        except RolloutNotFoundError as error:
            raise not_found("rollout", error) from error
        return Response(status_code=status.HTTP_204_NO_CONTENT)

    # --- ota: device dispatch ---------------------------------------------

    @application.post(
        "/api/v1/devices/{device_id}/ota/check",
        response_model=OtaCheckResponse,
        dependencies=[authorization],
    )
    def check_device_ota(
        device_id: str, request: OtaCheckRequest
    ) -> OtaCheckResponse:
        try:
            store.get_device(device_id)
        except DeviceNotFoundError as error:
            raise missing_device(error) from error
        return ota_engine.evaluate(
            device_id, request.product_id, request.hw_version, request.fw_version
        )

    @application.post(
        "/api/v1/devices/{device_id}/ota/progress",
        response_model=OtaUpdateRecord,
        dependencies=[authorization],
    )
    def report_device_ota(
        device_id: str, request: OtaProgressRequest
    ) -> OtaUpdateRecord:
        try:
            return store.record_ota_result(
                device_id, request.status, request.fw_version, request.message_id
            )
        except DeviceNotFoundError as error:
            raise missing_device(error) from error
        except OtaUpdateNotFoundError as error:
            raise not_found("ota update", error) from error

    @application.get(
        "/api/v1/devices/{device_id}/ota/updates",
        response_model=list[OtaUpdateRecord],
    )
    def list_device_ota(
        device_id: str,
        principal: Principal = Depends(get_principal),
    ) -> list[OtaUpdateRecord]:
        enforce_device(principal, device_id)
        return store.list_ota_updates(device_id)

    application.state.store = store
    application.state.mqtt_bridge = bridge
    application.state.automation_engine = engine
    application.state.ota_engine = ota_engine
    return application


app = create_app()
