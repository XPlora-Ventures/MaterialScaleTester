import os
import queue

from flask import Blueprint, request, jsonify, send_file, Response, stream_with_context

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

    @bp.route("/api/plug/debug")
    def api_plug_debug():
        try:
            result = plug.list_devices()
            return jsonify({"ok": True, "devices": result})
        except Exception as e:
            return jsonify({"ok": False, "error": str(e)})

    @bp.route("/api/sd/files")
    def api_sd_files():
        q = device_mgr.start_sd_transfer()
        if q is None:
            return jsonify({"error": "SD transfer already in progress"}), 503
        try:
            device_mgr.send({"cmd": "list_files"})
            msg = q.get(timeout=8)
            if "error" in msg:
                return jsonify({"error": msg["error"]}), 502
            return jsonify(msg.get("files", []))
        except queue.Empty:
            return jsonify({"error": "device timeout"}), 504
        finally:
            device_mgr.end_sd_transfer()

    @bp.route("/api/sd/download/<path:filename>")
    def api_sd_download(filename):
        q = device_mgr.start_sd_transfer()
        if q is None:
            return jsonify({"error": "SD transfer already in progress"}), 503
        device_mgr.send({"cmd": "read_file", "name": "/" + filename})

        @stream_with_context
        def generate():
            try:
                while True:
                    try:
                        chunk = q.get(timeout=10)
                        if "error" in chunk:
                            break
                        yield chunk.get("data", "")
                        if chunk.get("done"):
                            break
                    except queue.Empty:
                        break
            finally:
                device_mgr.end_sd_transfer()

        return Response(
            generate(),
            mimetype="text/csv",
            headers={"Content-Disposition": f'attachment; filename="{filename}"'},
        )

    return bp
