"""BMCU channel-only HTTP interface."""

import asyncio
import re

from aiohttp import ClientError

DOMAIN = "bmcu_esp"
PATH = "/api/home-assistant/channels"


class DeviceError(Exception):
    """Device unreachable, disabled, or rejected the request."""


async def request(session, host, path=PATH, data=None):
    """Send one request; never replay a motor command on a redirect or timeout."""
    authority = f"[{host}]" if ":" in host else host
    try:
        async with asyncio.timeout(5), session.request(
            "GET" if data is None else "POST", f"http://{authority}{path}",
            data=data, allow_redirects=False,
        ) as response:
            body = await response.json()
            if not isinstance(body, dict):
                raise DeviceError("Invalid BMCU response")
            if not 200 <= response.status < 300 or (data is not None and body.get("ok") is not True):
                raise DeviceError(body.get("msg") or f"BMCU HTTP {response.status}")
            return body
    except (ClientError, TimeoutError, ValueError) as err:
        raise DeviceError(f"BMCU request failed: {err}") from err


async def snapshot(session, host, expected_id=None):
    """Read the channel list and verify device identity after address changes."""
    body = await request(session, host)
    uid, channels = body.get("id"), body.get("channels")
    if not isinstance(uid, str) or not re.fullmatch(r"[0-9a-f]{16}", uid) or (expected_id and uid != expected_id):
        raise DeviceError("Unexpected BMCU device ID")
    if not isinstance(channels, list) or len(channels) not in (4, 8, 12, 16) or not all(
        isinstance(ch, dict) and isinstance(ch.get("ready"), bool)
        and type(ch.get("timeout")) is int and 3 <= ch["timeout"] <= 30 for ch in channels
    ):
        raise DeviceError("Invalid BMCU channel list")
    return uid, channels
