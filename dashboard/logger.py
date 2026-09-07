import csv
import datetime
import threading


class CsvLogger:
    def __init__(self):
        self._file = None
        self._writer = None
        self._path = None
        self._lock = threading.Lock()

    @property
    def path(self) -> str | None:
        return self._path

    def start(self) -> tuple[bool, str]:
        with self._lock:
            if self._writer:
                return False, "already logging"
            ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            self._path = f"mst_{ts}.csv"
            self._file = open(self._path, "w", newline="")
            self._writer = csv.writer(self._file)
            self._writer.writerow([
                "timestamp", "state", "solenoid_humid", "solenoid_drier",
                "cycles", "target", "time_left_ms", "temp_c", "thermal",
            ])
        return True, self._path

    def stop(self):
        with self._lock:
            if self._file:
                self._file.close()
            self._file = self._writer = None

    def log_row(self, data: dict):
        with self._lock:
            if self._writer is None:
                return
            self._writer.writerow([
                datetime.datetime.now().isoformat(),
                data.get("state", ""),
                int(data.get("solenoid_humid", False)),
                int(data.get("solenoid_drier", False)),
                data.get("cycles", ""),
                data.get("target", ""),
                data.get("time_left_ms", ""),
                data.get("temp_c", ""),
                data.get("thermal", ""),
            ])
            self._file.flush()
