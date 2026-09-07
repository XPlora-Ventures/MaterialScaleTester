import os
import logging
import tinytuya

log = logging.getLogger(__name__)

_REGION    = os.environ.get("TUYA_REGION", "")
_KEY       = os.environ.get("TUYA_API_KEY", "")
_SECRET    = os.environ.get("TUYA_API_SECRET", "")
_DEVICE_ID = os.environ.get("TUYA_DEVICE_ID", "")


class PlugController:
    def _cloud(self):
        return tinytuya.Cloud(
            apiRegion=_REGION,
            apiKey=_KEY,
            apiSecret=_SECRET,
        )

    def _send(self, value: bool):
        if not all([_REGION, _KEY, _SECRET, _DEVICE_ID]):
            raise RuntimeError("Tuya env vars not set (TUYA_REGION, TUYA_API_KEY, TUYA_API_SECRET, TUYA_DEVICE_ID)")
        c = self._cloud()
        # v2.0 IoT Core — correct for Industry projects
        result = c.cloudrequest(
            f'cloud/thing/{_DEVICE_ID}/shadow/properties/issue',
            action='POST',
            post={'properties': {'switch_1': value}},
            ver='v2.0'
        )
        print(f"[PLUG] v2 {'on' if value else 'off'} -> {result}", flush=True)
        if isinstance(result, dict) and not result.get('Error') and result.get('success') is not False:
            return
        # Fallback: iot-03 Smart Home endpoint
        result2 = c.sendcommand(_DEVICE_ID, [{"code": "switch_1", "value": value}])
        print(f"[PLUG] iot-03 {'on' if value else 'off'} -> {result2}", flush=True)
        if isinstance(result2, dict) and result2.get('Error'):
            raise RuntimeError(result2.get('Payload') or str(result2))

    def turn_on(self):
        self._send(True)

    def turn_off(self):
        self._send(False)

    def list_devices(self):
        return self._cloud().getdevices()

    def is_on(self) -> bool:
        try:
            result = self._cloud().getstatus(_DEVICE_ID)
            log.info("plug getstatus result: %s", result)
            for item in result.get("result", []):
                if item.get("code") == "switch_1":
                    return bool(item["value"])
        except Exception as e:
            log.warning("plug is_on failed: %s", e)
        return False
