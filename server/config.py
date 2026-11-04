"""config.py — every setting of the Passport server, read from the environment.

On import, a dotenv-style file is loaded into os.environ (variables that are
already set win). The file is $PASSPORT_ENV_FILE, or `.env` next to this file.
See .env.example for every variable and what it does.
"""
import os
import re
from datetime import timedelta, timezone, tzinfo

HERE = os.path.dirname(os.path.abspath(__file__))


def load_env_file(path: str) -> None:
    """KEY=VALUE lines; '#' comments; optional single/double quotes. Never overrides."""
    if not path or not os.path.exists(path):
        return
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            k, v = k.strip(), v.strip()
            if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
                v = v[1:-1]
            os.environ.setdefault(k, v)


load_env_file(os.environ.get("PASSPORT_ENV_FILE") or os.path.join(HERE, ".env"))


def get(name: str, default: str = "") -> str:
    v = os.environ.get(name)
    return default if v is None or v == "" else v


def get_int(name: str, default: int) -> int:
    try:
        return int(get(name, str(default)))
    except ValueError:
        return default


def get_float(name: str, default: float) -> float:
    try:
        return float(get(name, str(default)))
    except ValueError:
        return default


def get_bool(name: str, default: bool = False) -> bool:
    v = get(name, "")
    if not v:
        return default
    return v.lower() in ("1", "true", "yes", "on")


def path(name: str, default: str) -> str:
    """A path setting; relative paths are taken relative to the server directory."""
    p = os.path.expanduser(get(name, default))
    return p if os.path.isabs(p) else os.path.join(HERE, p)


def tz_from(spec: str) -> tzinfo:
    """'+08:00' / '-05:30' / 'UTC' / an IANA name such as 'Europe/Berlin'."""
    spec = (spec or "UTC").strip()
    m = re.fullmatch(r"(?:UTC|GMT)?\s*([+-])(\d{1,2})(?::?(\d{2}))?", spec)
    if m:
        sign = -1 if m.group(1) == "-" else 1
        return timezone(sign * timedelta(hours=int(m.group(2)), minutes=int(m.group(3) or 0)))
    if spec.upper() in ("UTC", "GMT", "Z"):
        return timezone.utc
    from zoneinfo import ZoneInfo
    return ZoneInfo(spec)


# ------------------------------------------------------------------ shared settings
def state_dir() -> str:
    return path("PASSPORT_STATE_DIR", "state")


def tz() -> tzinfo:
    # The card day (Bingo) and timestamps use this zone. It must match the
    # time zone configured in the device firmware.
    return tz_from(get("PASSPORT_TZ", "+08:00"))


def device_id() -> str:
    return get("PASSPORT_DEVICE_ID", "passport-1")


def topic(name: str) -> str:
    """<prefix>/<device id>/<name>, e.g. passport/passport-1/command."""
    return f"{get('PASSPORT_TOPIC_PREFIX', 'passport')}/{device_id()}/{name}"


def ctl() -> str:
    """How event hints tell the companion to call the CLI."""
    return get("PASSPORT_CTL", "passportctl")


def mqtt_client(client_id: str):
    """A paho-mqtt v2 client configured from MQTT_* (not connected yet)."""
    import paho.mqtt.client as mqtt
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    if get("MQTT_USER"):
        c.username_pw_set(get("MQTT_USER"), get("MQTT_PASS"))
    if get_bool("MQTT_TLS"):
        c.tls_set(ca_certs=get("MQTT_CA_FILE") or None)
        if get_bool("MQTT_TLS_INSECURE"):
            c.tls_insecure_set(True)
    return c


def mqtt_connect(c, keepalive: int = 60):
    c.connect(get("MQTT_HOST", "127.0.0.1"), get_int("MQTT_PORT", 1883), keepalive)
