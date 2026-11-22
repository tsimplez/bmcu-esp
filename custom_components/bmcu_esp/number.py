"""Persistent per-channel automatic retract time."""

from homeassistant.components.number import NumberEntity, NumberMode
from homeassistant.const import EntityCategory, UnitOfTime
from homeassistant.exceptions import HomeAssistantError

from .entity import ChannelEntity, add_channels


async def async_setup_entry(hass, entry, async_add_entities):
    add_channels(entry, async_add_entities, lambda c, ch: [RetractTime(c, ch, "timeout", "退料时间")])


class RetractTime(ChannelEntity, NumberEntity):
    _attr_entity_category = EntityCategory.CONFIG
    _attr_mode = NumberMode.BOX
    _attr_native_min_value = 3
    _attr_native_max_value = 30
    _attr_native_step = 1
    _attr_native_unit_of_measurement = UnitOfTime.SECONDS

    @property
    def native_value(self):
        if self.channel < len(self.coordinator.data):
            return self.coordinator.data[self.channel]["timeout"]
        return None

    async def async_set_native_value(self, value):
        if not 3 <= value <= 30 or value != int(value):
            raise HomeAssistantError("退料时间必须是 3 到 30 的整数秒")
        await self.coordinator.command("timeout", {f"to{self.channel}": str(int(value))})
        await self.coordinator.async_request_refresh()
