from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager, suppress
from datetime import datetime, timedelta, timezone
from typing import Annotated

from fastapi import Depends, FastAPI, Header, HTTPException, status

from .config import Settings
from .models import (
    ClaimDeviceRequest,
    DesiredUpdateResponse,
    DeviceResponse,
    HealthResponse,
    OfflineRequest,
    RegisterDeviceRequest,
    ShadowPatchRequest,
    ShadowResponse,
)
from .mqtt_bridge import MqttBridge
from .storage import DeviceAlreadyClaimedError, DeviceNotFoundError, DeviceStore


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

    application.state.store = store
    application.state.mqtt_bridge = bridge
    return application


app = create_app()
