from __future__ import annotations

from typing import Callable, Protocol

from pydantic import JsonValue

from .models import AutomationAction, CommandResult, SceneAction
from .storage import DeviceNotFoundError, DeviceStore

PublishCallback = Callable[[str, dict[str, JsonValue]], bool]


class SupportsShadowMutation(Protocol):
    def update_desired(
        self, device_id: str, patch: dict[str, JsonValue], message_id: str | None
    ) -> tuple[object, bool]: ...


def _no_publish(_device_id: str, _state: dict[str, JsonValue]) -> bool:
    return False


class AutomationEngine:
    """Applies desired state for group/scene commands and reacts to reported updates."""

    def __init__(self, store: DeviceStore, publish: PublishCallback | None = None) -> None:
        self._store = store
        self._publish = publish or _no_publish

    def set_publisher(self, publish: PublishCallback) -> None:
        self._publish = publish

    def apply_desired(
        self,
        device_id: str,
        state: dict[str, JsonValue],
        message_id: str | None = None,
    ) -> CommandResult:
        try:
            _shadow, changed = self._store.update_desired(device_id, state, message_id)
        except DeviceNotFoundError:
            return CommandResult(device_id=device_id, changed=False, published=False, found=False)
        published = changed and self._publish(device_id, state)
        return CommandResult(
            device_id=device_id, changed=changed, published=published, found=True
        )

    def apply_actions(self, actions: list[SceneAction]) -> list[CommandResult]:
        return [self.apply_desired(action.device_id, action.state) for action in actions]

    def activate_scene(self, scene_id: str) -> list[CommandResult]:
        return self.apply_actions(self._store.get_scene_actions(scene_id))

    def command_group(
        self,
        group_id: str,
        state: dict[str, JsonValue],
        message_id: str | None = None,
    ) -> list[CommandResult]:
        member_ids = self._store.list_group_member_ids(group_id)
        results: list[CommandResult] = []
        for index, member_id in enumerate(member_ids):
            scoped = None if message_id is None else f"{message_id}:{index}"
            results.append(self.apply_desired(member_id, state, scoped))
        return results

    def run_action(self, action: AutomationAction) -> list[CommandResult]:
        if action.type == "device":
            assert action.device_id is not None and action.state is not None
            return [self.apply_desired(action.device_id, action.state)]
        if action.type == "group":
            assert action.group_id is not None and action.state is not None
            return self.command_group(action.group_id, action.state)
        assert action.scene_id is not None
        return self.activate_scene(action.scene_id)

    def on_reported(self, device_id: str) -> list[CommandResult]:
        automations = self._store.list_enabled_automations_for(device_id)
        if not automations:
            return []
        reported = self._store.get_shadow(device_id).reported
        results: list[CommandResult] = []
        for automation in automations:
            trigger = automation.trigger
            if trigger.field not in reported:
                continue
            if reported[trigger.field] != trigger.equals:
                continue
            results.extend(self.run_action(automation.action))
        return results
