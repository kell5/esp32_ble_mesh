from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager, suppress
from datetime import datetime, timedelta, timezone
from typing import Annotated

from fastapi import Depends, FastAPI, Header, HTTPException, Response, status

from .automation import AutomationEngine
from .config import Settings
from .models import (
    ApplyResponse,
    AutomationRequest,
    AutomationResponse,
    AutomationUpdateRequest,
    ClaimDeviceRequest,
    DesiredUpdateResponse,
    DeviceResponse,
    GroupCommandRequest,
    GroupRequest,
    GroupResponse,
    GroupUpdateRequest,
    HealthResponse,
    OfflineRequest,
    RegisterDeviceRequest,
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
from .storage import (
    AutomationNotFoundError,
    DeviceAlreadyClaimedError,
    DeviceNotFoundError,
    DeviceStore,
    GroupNotFoundError,
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

    @application.post(
        "/api/v1/devices",
        response_model=DeviceResponse,
        status_code=status.HTTP_201_CREATED,
        dependencies=[authorization],
    )
    def register_device(request: RegisterDeviceRequest) -> DeviceResponse:
        return store.register_device(
            request.device_id, request.type, request.name, request.metadata
        )

    @application.get(
        "/api/v1/devices/{device_id}",
        response_model=DeviceResponse,
        dependencies=[authorization],
    )
    def get_device(device_id: str) -> DeviceResponse:
        try:
            return store.get_device(device_id)
        except DeviceNotFoundError as error:
            raise missing_device(error) from error

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
        dependencies=[authorization],
    )
    def get_shadow(device_id: str) -> ShadowResponse:
        try:
            return store.get_shadow(device_id)
        except DeviceNotFoundError as error:
            raise missing_device(error) from error

    @application.patch(
        "/api/v1/devices/{device_id}/shadow/desired",
        response_model=DesiredUpdateResponse,
        dependencies=[authorization],
    )
    def update_desired(device_id: str, request: ShadowPatchRequest) -> DesiredUpdateResponse:
        try:
            shadow, changed = store.update_desired(device_id, request.state, request.message_id)
        except DeviceNotFoundError as error:
            raise missing_device(error) from error
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

    application.state.store = store
    application.state.mqtt_bridge = bridge
    application.state.automation_engine = engine
    return application


app = create_app()
