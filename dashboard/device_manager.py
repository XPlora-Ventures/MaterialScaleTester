"""Manages the persistent WebSocket connection from the ESP32."""

import json
import queue
import threading


class DeviceManager:
    def __init__(self):
        self._ws = None          # the flask-sock WebSocket object
        self._lock = threading.Lock()
        self._listeners = []
        self._last_telemetry = {}
        self._sd_queue = None    # set during an SD file transfer

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

    def start_sd_transfer(self) -> "queue.Queue | None":
        with self._lock:
            if self._sd_queue is not None:
                return None  # already in use
            self._sd_queue = queue.Queue()
            return self._sd_queue

    def end_sd_transfer(self):
        with self._lock:
            self._sd_queue = None

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
        msg_type = msg.get("type")
        if msg_type in ("sd_files", "sd_chunk"):
            with self._lock:
                if self._sd_queue is not None:
                    self._sd_queue.put(msg)
            return
        if msg_type is None:    # only persist regular telemetry, not typed events
            self._last_telemetry = msg
        for cb in self._listeners:
            try:
                cb(msg)
            except Exception:
                pass
