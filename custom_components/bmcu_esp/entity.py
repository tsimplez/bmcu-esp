"""Channel discovery and common device identity."""

from homeassistant.core import callback
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .api import DOMAIN


def add_channels(entry, add_entities, factory):
    """Add newly enabled channels; retain removed IDs as unavailable for automations."""
    coordinator = entry.runtime_data
    known = 0

    @callback
    def update():
        nonlocal known
        count = len(coordinator.data)
        if count > known:
            add_entities([entity for ch in range(known, count) for entity in factory(coordinator, ch)])
            known = count

    update()
    entry.async_on_unload(coordinator.async_add_listener(update))


class ChannelEntity(CoordinatorEntity):
    _attr_has_entity_name = True

    def __init__(self, coordinator, channel, key, label):
        super().__init__(coordinator)
        self.channel = channel
        entry = coordinator.config_entry
        self._attr_unique_id = f"{entry.unique_id}_{channel}_{key}"
        self._attr_name = f"通道 {channel + 1} {label}"
        self._attr_device_info = DeviceInfo(identifiers={(DOMAIN, entry.unique_id)}, name=entry.title)

    @property
    def available(self):
        return super().available and self.channel < len(self.coordinator.data)
