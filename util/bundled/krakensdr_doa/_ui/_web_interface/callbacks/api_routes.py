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


@app.server.route("/api/doa/dump", methods=["GET", "POST"])
async def api_doa_dump():
    # POST {"n_frames": N} arms capture of the next N processed VFO frames
    # (per-channel channelized IQ + the DOA algorithm's own correlation
    # matrix + resulting angle/confidence/frequency) to an .npz file under
    # _share/records/doa_frames/ -- reachable over the existing PHP static
    # file server on :8081 (share_url in the GET response, once done) without
    # any extra code, same as everything else already served from _share.
    # For diagnosing "DOA looks fine on a static source but goes random when
    # moved" -- capture N frames spanning a physical move of the source, then
    # check offline whether it's the tracked frequency jumping (vfo_freq
    # column) or the per-channel phase relationships in `processed_signal`/`R`
    # themselves becoming unstable.
    sp = web_interface.module_signal_processor
    if request.method == "POST":
        body = await request.get_json(silent=True) or {}
        n_frames = int(body.get("n_frames", 100))
        sp.start_frame_dump(n_frames)
        return jsonify({"started": True, "status": sp.frame_dump_status}), 202

    return jsonify(sp.frame_dump_status)
