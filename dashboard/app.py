"""Material Scale Tester Dashboard — entry point."""

__version__ = "1.0.0"

import os
import threading
import webbrowser

from flask import Flask, render_template
from flask_socketio import SocketIO

from serial_manager import SerialManager
from logger import CsvLogger
from routes import create_blueprint

app = Flask(__name__, template_folder="templates")
socketio = SocketIO(app, cors_allowed_origins="*")

# ── Shutdown on tab close ─────────────────────────────────────────────────────

_shutdown_timer = None
_shutdown_timer_lock = threading.Lock()

def _schedule_shutdown():
    global _shutdown_timer
    def _do_shutdown():
        os._exit(0)
    with _shutdown_timer_lock:
        _shutdown_timer = threading.Timer(3.0, _do_shutdown)
        _shutdown_timer.daemon = True
        _shutdown_timer.start()

def _cancel_shutdown():
    global _shutdown_timer
    with _shutdown_timer_lock:
        if _shutdown_timer is not None:
            _shutdown_timer.cancel()
            _shutdown_timer = None

@socketio.on("connect")
def on_connect():
    _cancel_shutdown()

@socketio.on("disconnect")
def on_disconnect():
    _schedule_shutdown()

# ── Subsystems ────────────────────────────────────────────────────────────────

serial_mgr = SerialManager()
csv_logger = CsvLogger()

def _on_telemetry(data: dict):
    socketio.emit("telemetry", data)
    csv_logger.log_row(data)

serial_mgr.add_listener(_on_telemetry)

# ── Routes ────────────────────────────────────────────────────────────────────

app.register_blueprint(create_blueprint(serial_mgr, csv_logger))

@app.route("/")
def index():
    return render_template("index.html")

# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    webbrowser.open("http://localhost:8080")
    socketio.run(app, host="0.0.0.0", port=8080, debug=False, allow_unsafe_werkzeug=True)
