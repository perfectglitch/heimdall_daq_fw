# isort: off
from maindash import app, web_interface

# isort: on

import time

from kraken_sdr_signal_processor import reduce_spectrum
from quart import jsonify, request
from utils import start_wideband_sweep

# Plain REST endpoints, independent of the dash_devices websocket/push
# machinery and of en_spectrum/en_doa page-gating -- they read straight off
# the SignalProcessor thread's live attributes, which update continuously
# from the moment the process starts regardless of whether any browser is
# connected or which page it's on. See utils.compute_health_status for why
# that independence matters.


@app.server.route("/api/health")
async def api_health():
    return jsonify(web_interface.health_status)


@app.server.route("/api/spectrum")
async def api_spectrum():
    sp = web_interface.module_signal_processor
    spectrum = sp.spectrum
    if spectrum is None:
        return jsonify({"available": False, "timestamp": time.time()})

    reduced = reduce_spectrum(spectrum, sp.spectrum_plot_size, sp.channel_number)
    return jsonify(
        {
            "available": True,
            "freqs_hz": reduced[0, :].tolist(),
            "channels_db": [reduced[ch, :].tolist() for ch in range(1, sp.channel_number + 1)],
            "timestamp": time.time(),
        }
    )


@app.server.route("/api/doa")
async def api_doa():
    sp = web_interface.module_signal_processor
    # These four lists are index-aligned per active VFO and get cleared/
    # rebuilt together once per processed frame (see the *_list.clear() calls
    # in run()) -- zip() naturally handles the empty case (no VFO currently
    # over squelch this frame) by yielding nothing.
    vfos = [
        {
            "freq_hz": freq,
            "angle_deg": float(theta_0),
            "confidence": float(confidence),
            "power_dbm": float(power),
            "spectrum": doa_result_log.tolist(),
        }
        for freq, theta_0, confidence, power, doa_result_log in zip(
            sp.freq_list,
            sp.theta_0_list,
            sp.confidence_list,
            sp.max_power_level_list,
            sp.doa_result_log_list,
        )
    ]
    return jsonify(
        {
            "available": bool(vfos),
            "vfos": vfos,
            "timestamp": time.time(),
        }
    )


@app.server.route("/api/sweep", methods=["GET", "POST"])
async def api_sweep():
    # POST triggers a new sweep -- same effect as the UI button (see
    # utils.start_wideband_sweep for why it stops/restarts the whole DAQ
    # chain around the sweep itself). GET just reports current status/result,
    # safe to poll from a script without side effects.
    if request.method == "POST":
        if web_interface.sweep_running:
            return jsonify({"started": False, "reason": "sweep already running", "status": web_interface.sweep_status}), 409
        start_wideband_sweep(app, web_interface)
        return jsonify({"started": True, "status": web_interface.sweep_status}), 202

    return jsonify(
        {
            "running": web_interface.sweep_running,
            "status": web_interface.sweep_status,
            "result": web_interface.sweep_result,
        }
    )
