import os
import json
import time
import hmac
import uuid
import hashlib
import logging
import requests
import tinytuya

log = logging.getLogger(__name__)

_REGION    = os.environ.get("TUYA_REGION", "us")
_KEY       = os.environ.get("TUYA_API_KEY", "")
_SECRET    = os.environ.get("TUYA_API_SECRET", "")
_DEVICE_ID = os.environ.get("TUYA_DEVICE_ID", "")

_BASE_URL = {
    "us": "https://openapi.tuyaus.com",
    "eu": "https://openapi.tuyaeu.com",
    "cn": "https://openapi.tuyacn.com",
    "in": "https://openapi.tuyain.com",
}.get(_REGION, "https://openapi.tuyaus.com")


def _make_headers(token, method, path, body=""):
    t = str(int(time.time() * 1000))
    nonce = uuid.uuid4().hex
    content_hash = hashlib.sha256(body.encode()).hexdigest()
    str_to_sign = f"{method}\n{content_hash}\n\n{path}"
    payload = _KEY + token + t + nonce + str_to_sign
    sign = hmac.new(_SECRET.encode(), payload.encode(), hashlib.sha256).hexdigest().upper()
    headers = {"client_id": _KEY, "t": t, "nonce": nonce, "sign": sign, "sign_method": "HMAC-SHA256"}
    if token:
        headers["access_token"] = token
    return headers


def _get_token():
    path = "/v1.0/token?grant_type=1"
    r = requests.get(_BASE_URL + path, headers=_make_headers("", "GET", path), timeout=10)
    data = r.json()
    print(f"[PLUG] token -> {data}", flush=True)
    if not data.get("success"):
        raise RuntimeError(f"Tuya token failed: {data.get('msg', data)}")
    return data["result"]["access_token"]


def _send_command(token, value: bool):
    path = f"/v1.0/iot-03/devices/{_DEVICE_ID}/commands"
    body = json.dumps({"commands": [{"code": "switch_1", "value": value}]})
    headers = _make_headers(token, "POST", path, body)
    headers["Content-Type"] = "application/json"
    r = requests.post(_BASE_URL + path, headers=headers, data=body, timeout=10)
    result = r.json()
    print(f"[PLUG] command {'on' if value else 'off'} -> {result}", flush=True)
    return result


def _cloud():
    try:
        return tinytuya.Cloud(apiRegion=_REGION, apiKey=_KEY, apiSecret=_SECRET, new_sign_algorithm=True)
    except TypeError:
        return tinytuya.Cloud(apiRegion=_REGION, apiKey=_KEY, apiSecret=_SECRET)


class PlugController:
    def _send(self, value: bool):
        if not all([_KEY, _SECRET, _DEVICE_ID]):
            raise RuntimeError("Tuya env vars not set (TUYA_API_KEY, TUYA_API_SECRET, TUYA_DEVICE_ID)")
        token = _get_token()
        result = _send_command(token, value)
        if not result.get("success"):
            raise RuntimeError(result.get("msg") or str(result))

    def turn_on(self):
        self._send(True)

    def turn_off(self):
        self._send(False)

    def is_on(self) -> bool:
        try:
            result = _cloud().getstatus(_DEVICE_ID)
            for item in result.get("result", []):
                if item.get("code") == "switch_1":
                    return bool(item["value"])
        except Exception as e:
            log.warning("plug is_on failed: %s", e)
        return False

    def list_devices(self):
        return _cloud().getdevices()
