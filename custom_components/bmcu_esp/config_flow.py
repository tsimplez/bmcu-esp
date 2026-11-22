"""Discover BMCUs or connect to a manually specified LAN host."""

from ipaddress import IPv6Address
import re

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .api import DOMAIN, DeviceError, snapshot


class ConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def _identify(self, host):
        uid, _ = await snapshot(async_get_clientsession(self.hass), host)
        await self.async_set_unique_id(uid)
        self._abort_if_unique_id_configured(updates={"host": host})
        self.host = host
        self.title = f"BMCU-ESP {uid[-6:]}"

    async def async_step_user(self, user_input=None):
        errors = {}
        if user_input is not None:
            host = user_input["host"].strip().lower()
            if ":" in host and re.fullmatch(r"[0-9a-f:.]+", host):
                try:
                    host = str(IPv6Address(host))
                except ValueError:
                    errors["host"] = "invalid_host"
            elif not re.fullmatch(r"[a-z0-9][a-z0-9.\-]{0,252}", host):
                errors["host"] = "invalid_host"
            if not errors:
                try:
                    await self._identify(host)
                except DeviceError:
                    errors["base"] = "cannot_connect"
                else:
                    return self.async_create_entry(title=self.title, data={"host": host})
        return self.async_show_form(step_id="user", data_schema=vol.Schema({vol.Required("host"): str}), errors=errors)

    async def async_step_zeroconf(self, discovery_info):
        try:
            await self._identify(str(discovery_info.ip_address))
        except DeviceError:
            return self.async_abort(reason="cannot_connect")
        self.context["title_placeholders"] = {"name": self.title}
        return await self.async_step_confirm()

    async def async_step_confirm(self, user_input=None):
        if user_input is not None:
            return self.async_create_entry(title=self.title, data={"host": self.host})
        return self.async_show_form(step_id="confirm", data_schema=vol.Schema({}),
                                    description_placeholders={"name": self.title, "host": self.host})
