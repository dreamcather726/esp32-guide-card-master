import asyncio
from typing import Dict, Iterable, Optional, Set, TYPE_CHECKING

if TYPE_CHECKING:
    from core.connection import ConnectionHandler


_connections: Dict[str, "ConnectionHandler"] = {}
_connection_keys: Dict[int, Set[str]] = {}
_lock = asyncio.Lock()


def _identity_variants(device_id: str) -> Set[str]:
    if not device_id:
        return set()

    value = str(device_id).strip()
    if not value:
        return set()

    variants = {value, value.lower(), value.upper()}
    compact = value.replace(":", "").replace("-", "").strip()
    if compact and compact != value:
        variants.update({compact, compact.lower(), compact.upper()})
    return {item for item in variants if item}


def _collect_identity_keys(device_id: str, aliases: Optional[Iterable[str]] = None) -> Set[str]:
    keys = _identity_variants(device_id)
    if aliases:
        for alias in aliases:
            keys.update(_identity_variants(alias))
    return keys


async def register_connection(device_id: str, conn: "ConnectionHandler", aliases: Optional[Iterable[str]] = None) -> None:
    keys = _collect_identity_keys(device_id, aliases)
    if not keys:
        return

    async with _lock:
        conn_key = id(conn)
        old_keys = _connection_keys.get(conn_key, set())
        for old_key in old_keys - keys:
            if _connections.get(old_key) is conn:
                _connections.pop(old_key, None)
        for key in keys:
            _connections[key] = conn
        _connection_keys[conn_key] = keys


async def register_connection_aliases(conn: "ConnectionHandler", aliases: Iterable[str]) -> None:
    keys = _collect_identity_keys(getattr(conn, "device_id", ""), aliases)
    if not keys:
        return

    async with _lock:
        conn_key = id(conn)
        keys.update(_connection_keys.get(conn_key, set()))
        for key in keys:
            _connections[key] = conn
        _connection_keys[conn_key] = keys


async def unregister_connection(device_id: str, conn: "ConnectionHandler") -> None:
    async with _lock:
        conn_key = id(conn)
        keys = _connection_keys.pop(conn_key, None)
        if not keys:
            keys = _collect_identity_keys(device_id)
        for key in keys:
            if _connections.get(key) is conn:
                _connections.pop(key, None)


async def get_connection(device_id: str) -> Optional["ConnectionHandler"]:
    async with _lock:
        for key in _identity_variants(device_id):
            conn = _connections.get(key)
            if conn is not None:
                return conn
        return None
