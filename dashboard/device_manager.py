"""Manages the persistent WebSocket connection from the ESP32."""

import json
import threading


class DeviceManager:
    def __init__(self):
        self._ws = None          # the flask-sock WebSocket object
        self._lock = threading.Lock()
        self._listeners = []
        self._last_telemetry = {}

    def add_listener(self, cb):
        self._listeners.append(cb)

    def attach(self, ws):
        with self._lock:
            self._ws = ws

    def detach(self):
        with self._lock:
            self._ws = None

    @property
    def connected(self) -> bool:
        with self._lock:
            return self._ws is not None

    @property
    def last_telemetry(self) -> dict:
        return self._last_telemetry

    def send(self, obj: dict):
        with self._lock:
            ws = self._ws
        if ws is None:
            return
        try:
            ws.send(json.dumps(obj))
        except Exception:
            pass

    def on_message(self, data: str):
        try:
            msg = json.loads(data)
        except json.JSONDecodeError:
            return
        if "type" not in msg:   # only persist regular telemetry, not typed events
            self._last_telemetry = msg
        for cb in self._listeners:
            try:
                cb(msg)
            except Exception:
                pass
