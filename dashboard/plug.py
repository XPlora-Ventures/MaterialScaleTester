import os
import tinytuya

_REGION    = os.environ["TUYA_REGION"]     # e.g. "us", "eu", "cn", "in"
_KEY       = os.environ["TUYA_API_KEY"]
_SECRET    = os.environ["TUYA_API_SECRET"]
_DEVICE_ID = os.environ["TUYA_DEVICE_ID"]


class PlugController:
    def _cloud(self):
        return tinytuya.Cloud(
            apiRegion=_REGION,
            apiKey=_KEY,
            apiSecret=_SECRET,
        )

    def turn_on(self):
        self._cloud().sendcommand(_DEVICE_ID, [{"code": "switch_1", "value": True}])

    def turn_off(self):
        self._cloud().sendcommand(_DEVICE_ID, [{"code": "switch_1", "value": False}])

    def is_on(self) -> bool:
        try:
            result = self._cloud().getstatus(_DEVICE_ID)
            for item in result.get("result", []):
                if item.get("code") == "switch_1":
                    return bool(item["value"])
        except Exception:
            pass
        return False
