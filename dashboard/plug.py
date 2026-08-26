import tinytuya

_DEVICE_ID = "eb91ec27955ebdee6emm4e"
_LOCAL_KEY  = ";p-v6{(o>d:/W9&s"
_IP         = "192.168.0.48"
_VERSION    = 3.3


class PlugController:
    def _dev(self):
        d = tinytuya.OutletDevice(_DEVICE_ID, _IP, _LOCAL_KEY)
        d.set_version(_VERSION)
        return d

    def turn_on(self):
        self._dev().turn_on()

    def turn_off(self):
        self._dev().turn_off()

    def is_on(self) -> bool:
        try:
            data = self._dev().status()
            return bool(data.get("dps", {}).get("1", False))
        except Exception:
            return False
