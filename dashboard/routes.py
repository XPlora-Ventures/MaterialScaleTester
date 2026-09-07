import os

from flask import Blueprint, request, jsonify, send_file

from device_manager import DeviceManager
from logger import CsvLogger
from plug import PlugController


def create_blueprint(device_mgr: DeviceManager, csv_logger: CsvLogger, plug: PlugController) -> Blueprint:
    bp = Blueprint("api", __name__)

    @bp.route("/api/status")
    def api_status():
        return jsonify({
            "device_connected": device_mgr.connected,
            "telemetry": device_mgr.last_telemetry,
        })

    @bp.route("/api/command", methods=["POST"])
    def api_command():
        device_mgr.send(request.json or {})
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
