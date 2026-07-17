from __future__ import annotations

import uuid
from typing import Callable

from .models import FirmwareResponse, OtaCheckResponse
from .storage import DeviceNotFoundError, DeviceStore, OtaUpdateNotFoundError

PublishOta = Callable[[str, str, FirmwareResponse, str], bool]


def _no_publish(
    _device_id: str, _device_class: str, _firmware: FirmwareResponse, _message_id: str
) -> bool:
    return False


class OtaEngine:
    """Resolves the firmware a device should run and dispatches OTA commands."""

    def __init__(self, store: DeviceStore, publish: PublishOta | None = None) -> None:
        self._store = store
        self._publish = publish or _no_publish

    def set_publisher(self, publish: PublishOta) -> None:
        self._publish = publish

    def _device_class(self, device_id: str) -> str:
        try:
            device = self._store.get_device(device_id)
        except DeviceNotFoundError:
            return "device"
        class_value = device.metadata.get("class")
        if isinstance(class_value, str) and class_value:
            return class_value
        return device.type

    def evaluate(
        self,
        device_id: str,
        product_id: str,
        hw_version: str,
        current_fw_version: str,
    ) -> OtaCheckResponse:
        firmware, reason = self._store.select_ota_target(
            device_id, product_id, hw_version, current_fw_version
        )
        if firmware is None:
            return OtaCheckResponse(
                dispatched=False,
                published=False,
                reason=reason,
                target=None,
                update=None,
            )
        message_id = f"ota-{uuid.uuid4().hex}"
        record, created = self._store.begin_ota_dispatch(device_id, firmware, message_id)
        if not created:
            return OtaCheckResponse(
                dispatched=False,
                published=False,
                reason="already_dispatched",
                target=firmware,
                update=record,
            )
        published = self._publish(
            device_id, self._device_class(device_id), firmware, record.message_id or message_id
        )
        return OtaCheckResponse(
            dispatched=True,
            published=published,
            reason="dispatched",
            target=firmware,
            update=record,
        )

    def on_version_report(
        self,
        device_id: str,
        product_id: str,
        hw_version: str,
        fw_version: str,
    ) -> None:
        try:
            self._store.record_ota_result(device_id, "success", fw_version)
        except (DeviceNotFoundError, OtaUpdateNotFoundError):
            pass
        self.evaluate(device_id, product_id, hw_version, fw_version)
