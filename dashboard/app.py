"""Material Scale Tester Dashboard — entry point."""

__version__ = "2.0.0"

import eventlet
eventlet.monkey_patch()

import os

from flask import Flask, render_template, session, redirect, url_for, request
from flask_socketio import SocketIO
from flask_sock import Sock

from device_manager import DeviceManager
from logger import CsvLogger
from plug import PlugController
from routes import create_blueprint

app = Flask(__name__, template_folder="templates")
app.config["SECRET_KEY"] = os.environ.get("SECRET_KEY", "dev")
socketio = SocketIO(app, cors_allowed_origins="*")
sock = Sock(app)

_LOGIN_USER = os.environ.get("DASHBOARD_USER", "admin")
_LOGIN_PASS = os.environ.get("DASHBOARD_PASS", "changeme")

_PUBLIC_PATHS = {"/login", "/ws/device"}

@app.before_request
def require_login():
    if request.path in _PUBLIC_PATHS or request.path.startswith("/socket.io"):
        return
    if not session.get("authed"):
        return redirect(url_for("login"))

@app.route("/login", methods=["GET", "POST"])
def login():
    error = None
    if request.method == "POST":
        if (request.form.get("username") == _LOGIN_USER and
                request.form.get("password") == _LOGIN_PASS):
            session["authed"] = True
            return redirect(url_for("index"))
        error = "Invalid username or password."
    return render_template("login.html", error=error)

@app.route("/logout")
def logout():
    session.clear()
    return redirect(url_for("login"))

device_mgr = DeviceManager()
csv_logger = CsvLogger()
plug_ctrl  = PlugController()

def _on_telemetry(data: dict):
    socketio.emit("telemetry", data)
    csv_logger.log_row(data)

device_mgr.add_listener(_on_telemetry)

# ── ESP32 WebSocket endpoint ──────────────────────────────────────────────────

@sock.route("/ws/device")
def device_ws(ws):
    device_mgr.attach(ws)
    socketio.emit("device_connected", True)
    try:
        while True:
            msg = ws.receive()
            if msg is None:
                break
            device_mgr.on_message(msg)
    finally:
        device_mgr.detach()
        socketio.emit("device_connected", False)

# ── Routes ────────────────────────────────────────────────────────────────────

app.register_blueprint(create_blueprint(device_mgr, csv_logger, plug_ctrl))

@app.route("/")
def index():
    return render_template("index.html")

# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    port = int(os.environ.get("PORT", 8080))
    socketio.run(app, host="0.0.0.0", port=port, debug=False, allow_unsafe_werkzeug=True)
