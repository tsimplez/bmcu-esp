"""Momentary feed and retract controls."""

from homeassistant.components.button import ButtonEntity

from .entity import ChannelEntity, add_channels


async def async_setup_entry(hass, entry, async_add_entities):
    add_channels(entry, async_add_entities, lambda c, ch: [MotorButton(c, ch, d) for d in (1, -1)])


class MotorButton(ChannelEntity, ButtonEntity):
    def __init__(self, coordinator, channel, direction):
        super().__init__(coordinator, channel, "feed" if direction == 1 else "retract",
                         "进料" if direction == 1 else "退料")
        self.direction = direction

    @property
    def available(self):
        return super().available and self.coordinator.data[self.channel]["ready"]

    async def async_press(self):
        await self.coordinator.command("motor", {"motor": str(self.channel), "dir": str(self.direction)})
