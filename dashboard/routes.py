import os
import socket as _socket

from flask import Blueprint, request, jsonify, send_file

from serial_manager import SerialManager
from logger import CsvLogger
from plug import PlugController


def create_blueprint(serial_mgr: SerialManager, csv_logger: CsvLogger, plug: PlugController) -> Blueprint:
    bp = Blueprint("api", __name__)

    @bp.route("/api/ports")
    def api_ports():
        return jsonify(serial_mgr.list_ports())

    @bp.route("/api/connect", methods=["POST"])
    def api_connect():
        body = request.json or {}
        port = body.get("port", "")
        baud = int(body.get("baud", 115200))
        try:
            serial_mgr.connect(port, baud)
            return jsonify({"ok": True})
        except Exception as e:
            return jsonify({"ok": False, "error": str(e)})

    @bp.route("/api/disconnect", methods=["POST"])
    def api_disconnect():
        serial_mgr.disconnect()
        return jsonify({"ok": True})

    @bp.route("/api/command", methods=["POST"])
    def api_command():
        serial_mgr.send(request.json or {})
        return jsonify({"ok": True})

    @bp.route("/api/log/start", methods=["POST"])
    def api_log_start():
        ok, result = csv_logger.start()
        if ok:
            return jsonify({"ok": True, "file": result})
        return jsonify({"ok": False, "error": result})

    @bp.route("/api/log/stop", methods=["POST"])
    def api_log_stop():
        csv_logger.stop()
        return jsonify({"ok": True})

    @bp.route("/api/log/download")
    def api_log_download():
        path = csv_logger.path
        if path and os.path.exists(path):
            return send_file(path, as_attachment=True)
        return jsonify({"error": "no log"}), 404

    @bp.route("/api/ip")
    def api_ip():
        try:
            s = _socket.socket(_socket.AF_INET, _socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            s.close()
        except Exception:
            ip = "127.0.0.1"
        return jsonify({"ip": ip, "port": 8080})

    @bp.route("/api/plug/on", methods=["POST"])
    def api_plug_on():
        try:
            plug.turn_on()
            return jsonify({"ok": True, "state": True})
        except Exception as e:
            return jsonify({"ok": False, "error": str(e)})

    @bp.route("/api/plug/off", methods=["POST"])
    def api_plug_off():
        try:
            plug.turn_off()
            return jsonify({"ok": True, "state": False})
        except Exception as e:
            return jsonify({"ok": False, "error": str(e)})

    @bp.route("/api/plug/status")
    def api_plug_status():
        return jsonify({"on": plug.is_on()})

    return bp
