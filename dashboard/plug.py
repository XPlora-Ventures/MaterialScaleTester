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
        result = self._cloud().sendcommand(_DEVICE_ID, [{"code": "switch_1", "value": value}])
        print(f"[PLUG] sendcommand({'on' if value else 'off'}) -> {result}", flush=True)
        if isinstance(result, dict) and result.get("success") is False:
            raise RuntimeError(result.get("msg") or str(result))

    def turn_on(self):
        self._send(True)

    def turn_off(self):
        self._send(False)

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
