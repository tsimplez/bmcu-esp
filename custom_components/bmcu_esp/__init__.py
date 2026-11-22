"""BMCU-ESP local channel controls."""

from datetime import timedelta
import logging

from homeassistant.const import Platform
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import DOMAIN, PATH, DeviceError, request, snapshot

PLATFORMS = [Platform.BUTTON, Platform.NUMBER]


class Coordinator(DataUpdateCoordinator):
    """Poll all channels in one request and route writes to the paired device."""

    def __init__(self, hass, entry):
        super().__init__(hass, logging.getLogger(__name__), name=DOMAIN, config_entry=entry,
                         update_interval=timedelta(seconds=10))
        self.session = async_get_clientsession(hass)

    async def _async_update_data(self):
        try:
            _, channels = await snapshot(self.session, self.config_entry.data["host"], self.config_entry.unique_id)
            return channels
        except DeviceError as err:
            raise UpdateFailed(str(err)) from err

    async def command(self, action, fields):
        path = f"{PATH}/{self.config_entry.unique_id}/{action}"
        try:
            await request(self.session, self.config_entry.data["host"], path, fields)
        except DeviceError as err:
            raise HomeAssistantError(str(err)) from err


async def async_setup_entry(hass, entry):
    coordinator = Coordinator(hass, entry)
    await coordinator.async_config_entry_first_refresh()
    entry.runtime_data = coordinator
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass, entry):
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
