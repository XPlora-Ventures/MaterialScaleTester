import serial
import serial.tools.list_ports
import threading
import json


class SerialManager:
    def __init__(self):
        self._ser = None
        self._lock = threading.Lock()
        self._listeners = []

    def add_listener(self, cb):
        self._listeners.append(cb)

    def connect(self, port: str, baud: int):
        with self._lock:
            if self._ser and self._ser.is_open:
                self._ser.close()
            self._ser = serial.Serial(port, baud, timeout=1)
        threading.Thread(target=self._reader, daemon=True).start()

    def disconnect(self):
        with self._lock:
            if self._ser:
                self._ser.close()
                self._ser = None

    def send(self, obj: dict):
        with self._lock:
            if self._ser and self._ser.is_open:
                self._ser.write((json.dumps(obj) + "\n").encode())

    def list_ports(self) -> list[str]:
        return [p.device for p in serial.tools.list_ports.comports()]

    def _reader(self):
        while True:
            with self._lock:
                s = self._ser
            if s is None:
                break
            try:
                line = s.readline().decode("utf-8", errors="ignore").strip()
            except Exception:
                break
            if not line:
                continue
            try:
                data = json.loads(line)
            except json.JSONDecodeError:
                continue
            for cb in self._listeners:
                try:
                    cb(data)
                except Exception:
                    pass
