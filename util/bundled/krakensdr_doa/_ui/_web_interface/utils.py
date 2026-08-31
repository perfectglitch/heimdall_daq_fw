import copy
import json
import os
import queue
import subprocess
import time
from configparser import ConfigParser
from math import inf
from threading import Thread, Timer

import numpy as np
import variables
from dash_devices.dependencies import Output
from kraken_sdr_signal_processor import DEFAULT_VFO_FIR_ORDER_FACTOR
from kraken_web_doa import plot_doa
from kraken_web_spectrum import plot_spectrum
from variables import (
    AGC_WARNING_DISABLED_STYLE,
    AGC_WARNING_ENABLED_STYLE,
    AUTO_GAIN_VALUE,
    DEFAULT_MAPPING_SERVER_ENDPOINT,
    HZ_TO_MHZ,
    daq_config_filename,
    doa_fig,
)

RED_COLOR = {"color": "#e74c3c"}


def read_config_file_dict(config_fname=daq_config_filename):
    parser = ConfigParser()
    found = parser.read([config_fname])
    ini_data = {}
    if not found:
        return None

    ini_data["config_name"] = parser.get("meta", "config_name")
    ini_data["num_ch"] = parser.getint("hw", "num_ch")
    ini_data["en_bias_tee"] = parser.get("hw", "en_bias_tee")
    ini_data["daq_buffer_size"] = parser.getint("daq", "daq_buffer_size")
    ini_data["sample_rate"] = parser.getint("daq", "sample_rate")
    # USRP-only, absent on rtlsdr configs -- fallback keeps this read from
    # blowing up on those (see the auto_vfo_exclude feature, which derives
    # the DC/LO-leakage spike's location from this instead of a value the
    # user would otherwise have to keep in sync by hand every time
    # sample_rate or lo_offset_frac changes).
    ini_data["lo_offset_frac"] = parser.getfloat("daq", "lo_offset_frac", fallback=0.0)
    ini_data["en_noise_source_ctr"] = parser.getint("daq", "en_noise_source_ctr")
    ini_data["cpi_size"] = parser.getint("pre_processing", "cpi_size")
    ini_data["decimation_ratio"] = parser.getint("pre_processing", "decimation_ratio")
    ini_data["fir_relative_bandwidth"] = parser.getfloat("pre_processing", "fir_relative_bandwidth")
    ini_data["fir_tap_size"] = parser.getint("pre_processing", "fir_tap_size")
    ini_data["fir_window"] = parser.get("pre_processing", "fir_window")
    ini_data["en_filter_reset"] = parser.getint("pre_processing", "en_filter_reset")
    ini_data["corr_size"] = parser.getint("calibration", "corr_size")
    ini_data["std_ch_ind"] = parser.getint("calibration", "std_ch_ind")
    ini_data["en_iq_cal"] = parser.getint("calibration", "en_iq_cal")
    ini_data["gain_lock_interval"] = parser.getint("calibration", "gain_lock_interval")
    ini_data["require_track_lock_intervention"] = parser.getint("calibration", "require_track_lock_intervention")
    ini_data["cal_track_mode"] = parser.getint("calibration", "cal_track_mode")
    ini_data["amplitude_cal_mode"] = parser.get("calibration", "amplitude_cal_mode")
    ini_data["cal_frame_interval"] = parser.getint("calibration", "cal_frame_interval")
    ini_data["cal_frame_burst_size"] = parser.getint("calibration", "cal_frame_burst_size")
    ini_data["amplitude_tolerance"] = parser.getint("calibration", "amplitude_tolerance")
    ini_data["phase_tolerance"] = parser.getint("calibration", "phase_tolerance")
    ini_data["maximum_sync_fails"] = parser.getint("calibration", "maximum_sync_fails")
    ini_data["iq_adjust_source"] = parser.get("calibration", "iq_adjust_source")
    ini_data["iq_adjust_amplitude"] = parser.get("calibration", "iq_adjust_amplitude")
    ini_data["iq_adjust_time_delay_ns"] = parser.get("calibration", "iq_adjust_time_delay_ns")

    ini_data["adpis_gains_init"] = parser.get("adpis", "adpis_gains_init")

    ini_data["out_data_iface_type"] = parser.get("data_interface", "out_data_iface_type")

    return ini_data


def set_clicked(web_interface, clickData):
    M = web_interface.module_receiver.M
    curveNumber = clickData["points"][0]["curveNumber"]

    if curveNumber >= M:
        vfo_idx = int((curveNumber - M) / 2)
        web_interface.selected_vfo = vfo_idx
        if web_interface.module_signal_processor.output_vfo >= 0:
            web_interface.module_signal_processor.output_vfo = vfo_idx
            return Output("output_vfo", "value", vfo_idx)
    else:
        idx = 0
        if web_interface.module_signal_processor.output_vfo >= 0:
            idx = max(web_interface.module_signal_processor.output_vfo, 0)
        else:
            idx = web_interface.selected_vfo
        web_interface.module_signal_processor.vfo_freq[idx] = int(clickData["points"][0]["x"])
        return Output(f"vfo_{idx}_freq", "value", web_interface.module_signal_processor.vfo_freq[idx] * HZ_TO_MHZ)
    return None


def fetch_dsp_data(app, web_interface, spectrum_fig, waterfall_fig):
    # FIX (memory/thread leak): this used to be a single iteration that
    # rescheduled itself via `Timer(0.01, fetch_dsp_data, ...).start()` --
    # every single cycle spawned a brand-new OS thread (~100/sec), and worse,
    # every reconnect (page reload, dropped-but-undetected websocket,
    # multiple tabs) called this function again from callbacks/main.py's
    # connect handler, starting a SECOND independent chain. Since
    # `web_interface.dsp_timer` is one attribute holding only the MOST
    # RECENT Timer, an earlier chain's Timer reference gets overwritten and
    # becomes permanently uncancellable -- it keeps spawning a new thread
    # every 10ms forever, with no way to stop it, for the remaining lifetime
    # of the process. Over a long session with many reconnects (confirmed:
    # this process reached ~14GB RSS after ~12h uptime on a 15GB-RAM
    # machine), these zombie chains stack up and never get cleaned up.
    #
    # Fix: exactly one persistent background thread for the process
    # lifetime, guarded by dsp_running so a duplicate start is a no-op
    # instead of spawning a second chain, and stopped by clearing the flag
    # (checked by the loop itself) rather than an unreliable Timer.cancel()
    # on a reference that may no longer point at every live chain.
    if getattr(web_interface, "dsp_thread", None) is not None and web_interface.dsp_thread.is_alive():
        web_interface.logger.debug("fetch_dsp_data: already running, not starting a second chain")
        return
    web_interface.dsp_running = True
    web_interface.dsp_thread = Thread(
        target=_dsp_data_loop, args=(app, web_interface, spectrum_fig, waterfall_fig), daemon=True
    )
    web_interface.dsp_thread.start()


def _dsp_data_loop(app, web_interface, spectrum_fig, waterfall_fig):
    while web_interface.dsp_running:
        _fetch_dsp_data_once(app, web_interface, spectrum_fig, waterfall_fig)
        time.sleep(0.01)


def _fetch_dsp_data_once(app, web_interface, spectrum_fig, waterfall_fig):
    daq_status_update_flag = 0
    spectrum_update_flag = 0
    doa_update_flag = 0
    # freq_update            = 0 #no_update
    #############################################
    #      Fetch new data from back-end ques    #
    #############################################
    try:
        # Fetch new data from the receiver module
        que_data_packet = web_interface.rx_data_que.get(False)
        for data_entry in que_data_packet:
            if data_entry[0] == "conn-ok":
                web_interface.daq_conn_status = 1
                daq_status_update_flag = 1
            elif data_entry[0] == "disconn-ok":
                web_interface.daq_conn_status = 0
                daq_status_update_flag = 1
            elif data_entry[0] == "config-ok":
                web_interface.daq_cfg_iface_status = 0
                daq_status_update_flag = 1
    except queue.Empty:
        # Handle empty queue here
        web_interface.logger.debug("Receiver module que is empty")
    except Exception:
        # DEBUG (temporary): same gap as the signal-processing queue block
        # below, one level earlier -- this is the FIRST thing fetch_dsp_data
        # does, so an exception here was killing the Timer chain before it
        # ever reached either of the two later guards.
        web_interface.logger.exception(
            "fetch_dsp_data: FAILED processing receiver-module queue entry"
        )
    else:
        pass
        # Handle task here and call q.task_done()
    if web_interface.daq_restart:  # Set by the restarting script
        daq_status_update_flag = 1
    # DEBUG (temporary): this block used to catch ONLY queue.Empty -- any
    # other exception while processing one queued entry (a bad/malformed
    # data_entry, whatever) would propagate out of fetch_dsp_data. The render
    # dispatch further down is already guarded (see the try/except right
    # before the Timer reschedule), so that no longer kills the Timer chain
    # outright -- but THIS block runs first and is what actually updates
    # web_interface.spectrum/doa_thetas/etc. An exception here, now that it
    # can no longer take the whole loop down, still means the assignment
    # (e.g. "web_interface.spectrum = data_entry[1]") for the CURRENT
    # que_data_packet just never happens, and every LATER entry in that same
    # packet is skipped too (the for-loop stops on the exception) -- but the
    # packet is already dequeued, so it's gone. If this is where the "console
    # keeps ticking, render never throws, but the plotted data stays frozen"
    # symptom comes from, it'll show up here.
    _debug_last_data_entry_kind = None
    try:
        # Fetch new data from the signal processing module
        que_data_packet = web_interface.sp_data_que.get(False)
        for data_entry in que_data_packet:
            _debug_last_data_entry_kind = data_entry[0]
            if data_entry[0] == "iq_header":
                daq_status_update_flag = 1
                web_interface.logger.debug("Iq header data fetched from signal processing que")
                iq_header = data_entry[1]
                # Unpack header
                web_interface.daq_frame_index = iq_header.cpi_index
                if iq_header.frame_type == iq_header.FRAME_TYPE_DATA:
                    web_interface.daq_frame_type = "Data"
                elif iq_header.frame_type == iq_header.FRAME_TYPE_DUMMY:
                    web_interface.daq_frame_type = "Dummy"
                elif iq_header.frame_type == iq_header.FRAME_TYPE_CAL:
                    web_interface.daq_frame_type = "Calibration"
                elif iq_header.frame_type == iq_header.FRAME_TYPE_TRIGW:
                    web_interface.daq_frame_type = "Trigger wait"
                elif iq_header.frame_type == iq_header.FRAME_TYPE_EMPTY:
                    web_interface.daq_frame_type = "Empty"
                    continue
                else:
                    web_interface.daq_frame_type = "Unknown"

                web_interface.daq_frame_sync = iq_header.check_sync_word()
                web_interface.daq_power_level = iq_header.adc_overdrive_flags
                web_interface.daq_sample_delay_sync = iq_header.delay_sync_flag
                web_interface.daq_iq_sync = iq_header.iq_sync_flag
                web_interface.daq_noise_source_state = iq_header.noise_source_state

                # if web_interface.daq_center_freq != iq_header.rf_center_freq/10**6:
                #    freq_update = 1

                web_interface.daq_center_freq = iq_header.rf_center_freq / 10**6
                web_interface.daq_adc_fs = iq_header.adc_sampling_freq / 10**6
                web_interface.daq_fs = iq_header.sampling_freq / 10**6
                web_interface.daq_cpi = (
                    int(iq_header.cpi_length * 10**3 / iq_header.sampling_freq) if iq_header.sampling_freq else 0
                )
                gain_list_str = ""

                for m in range(iq_header.active_ant_chs):
                    gain_list_str += str(iq_header.if_gains[m] / 10)
                    gain_list_str += ", "

                web_interface.daq_if_gains = gain_list_str[:-2]
            elif data_entry[0] == "update_rate":
                web_interface.daq_update_rate = data_entry[1]
            elif data_entry[0] == "latency":
                web_interface.daq_dsp_latency = data_entry[1] + web_interface.daq_cpi
            elif data_entry[0] == "max_amplitude":
                web_interface.max_amplitude = data_entry[1]
            elif data_entry[0] == "avg_powers":
                avg_powers_str = ""
                for avg_power in data_entry[1]:
                    avg_powers_str += "{:.1f}".format(avg_power)
                    avg_powers_str += ", "
                web_interface.avg_powers = avg_powers_str[:-2]
            elif data_entry[0] == "spectrum":
                web_interface.logger.debug("Spectrum data fetched from signal processing que")
                spectrum_update_flag = 1
                web_interface.spectrum = data_entry[1]
            elif data_entry[0] == "doa_thetas":
                web_interface.doa_thetas = data_entry[1]
                doa_update_flag = 1
                web_interface.doa_results = []
                web_interface.doa_labels = []
                web_interface.doas = []
                web_interface.max_doas_list = []
                web_interface.doa_confidences = []
                web_interface.logger.debug("DoA estimation data fetched from signal processing que")
            elif data_entry[0] == "DoA Result":
                web_interface.doa_results.append(data_entry[1])
                web_interface.doa_labels.append(data_entry[0])
            elif data_entry[0] == "DoA Max":
                web_interface.doas.append(data_entry[1])
            elif data_entry[0] == "DoA Confidence":
                web_interface.doa_confidences.append(data_entry[1])
            elif data_entry[0] == "DoA Max List":
                web_interface.max_doas_list = data_entry[1].copy()
            elif data_entry[0] == "DoA Squelch":
                web_interface.squelch_update = data_entry[1].copy()
            elif data_entry[0] == "VFO-0 Frequency":
                app.push_mods(
                    {
                        "vfo_0_freq": {"value": data_entry[1] * HZ_TO_MHZ},
                    }
                )
            else:
                web_interface.logger.warning("Unknown data entry: {:s}".format(data_entry[0]))
    except queue.Empty:
        # Handle empty queue here
        web_interface.logger.debug("Signal processing que is empty")
    except Exception:
        web_interface.logger.exception(
            "fetch_dsp_data: FAILED processing signal-processing queue entry kind=%s -- "
            "web_interface.spectrum/doa state will be stale until the next successful packet",
            _debug_last_data_entry_kind,
        )
    else:
        pass
        # Handle task here and call q.task_done()

    # fetch_dsp_data reschedules itself via the Timer below -- it's the sole
    # driver of every live UI update (spectrum, DOA, DAQ status), not just
    # this one page. An uncaught exception in any of the three render calls
    # below used to propagate out of this function, which killed the Timer
    # thread before it reached the reschedule line at the bottom -- silently
    # freezing ALL live updates (any page, not just the one that triggered
    # it) until the whole process was restarted, with no logged trace of what
    # broke. Guard each render call so a bug in one of them can't take down
    # the update loop itself, and log it so the next occurrence is
    # diagnosable instead of just "the UI stopped".
    try:
        if (
            web_interface.pathname == "/config" or web_interface.pathname == "/" or web_interface.pathname == "/init"
        ) and daq_status_update_flag:
            update_daq_status(app, web_interface)
        elif web_interface.pathname == "/spectrum" and spectrum_update_flag:
            plot_spectrum(app, web_interface, spectrum_fig, waterfall_fig)
        # or (web_interface.pathname == "/doa" and
        # web_interface.reset_doa_graph_flag):
        elif web_interface.pathname == "/doa" and doa_update_flag:
            plot_doa(app, web_interface, doa_fig)
    except Exception:
        web_interface.logger.exception(
            "fetch_dsp_data: render callback failed on pathname=%s (spectrum_update=%s, doa_update=%s, "
            "daq_status_update=%s, vfo_mode=%s, active_vfos=%s) -- continuing so live updates don't freeze",
            web_interface.pathname,
            spectrum_update_flag,
            doa_update_flag,
            daq_status_update_flag,
            getattr(web_interface.module_signal_processor, "vfo_mode", "?"),
            getattr(web_interface.module_signal_processor, "active_vfos", "?"),
        )


def fetch_gps_data(app, web_interface):
    # FIX (memory/thread leak): same duplicate-chain issue as fetch_dsp_data
    # above, just at a 50x lower rate (1/sec vs 100/sec) -- same fix shape.
    if getattr(web_interface, "gps_thread", None) is not None and web_interface.gps_thread.is_alive():
        web_interface.logger.debug("fetch_gps_data: already running, not starting a second chain")
        return
    web_interface.gps_running = True
    web_interface.gps_thread = Thread(target=_gps_data_loop, args=(app, web_interface), daemon=True)
    web_interface.gps_thread.start()


def _gps_data_loop(app, web_interface):
    while web_interface.gps_running:
        app.push_mods(
            {
                "body_gps_latitude": {"children": web_interface.module_signal_processor.latitude},
                "body_gps_longitude": {"children": web_interface.module_signal_processor.longitude},
                "body_gps_heading": {"children": web_interface.module_signal_processor.heading},
            }
        )
        time.sleep(1)


# Health check: process names that make up the DAQ acquisition chain. Any of
# these missing means that part of the chain has actually crashed/exited
# (e.g. a USRP driver crash), not just that it's momentarily busy.
DAQ_CORE_PROCESS_NAMES = [
    "usrp_daq.out",
    "rebuffer.out",
    "decimate.out",
    "delay_sync.py",
    "hw_controller.py",
]

# If the signal processor hasn't pulled a new frame out of shared memory in
# this long, treat the chain as stalled even if the processes are still
# technically running (e.g. blocked on a USB dropout).
HEALTH_STALE_THRESHOLD_S = 5.0


def _daq_core_processes_alive():
    """Returns {process_name: bool} by scanning /proc directly -- avoids
    spawning a pgrep subprocess once a second just to answer "is it still
    running"."""
    found = {name: False for name in DAQ_CORE_PROCESS_NAMES}
    try:
        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue
            try:
                with open(f"/proc/{pid}/cmdline", "rb") as f:
                    cmdline = f.read().decode(errors="ignore")
            except OSError:
                continue
            for name in DAQ_CORE_PROCESS_NAMES:
                if not found[name] and name in cmdline:
                    found[name] = True
    except OSError:
        pass
    return found


def compute_health_status(web_interface):
    """Single source of truth for DAQ health, read by both the UI badge
    push loop and the /api/health REST endpoint. Deliberately independent
    of en_spectrum/en_doa/browser-connection state -- see the module docstring
    note on fetch_dsp_data for why those are page-gated, which would make a
    health check built on top of them just as blind as the symptom we're
    fixing ("no updates" looking identical whether the USRP crashed or
    nobody's on that page)."""
    now = time.time()
    processes = _daq_core_processes_alive()
    processes_ok = all(processes.values())

    last_frame_time = getattr(web_interface.module_signal_processor, "last_frame_time", None)
    last_frame_age_s = (now - last_frame_time) if last_frame_time is not None else None
    frame_fresh = last_frame_age_s is not None and last_frame_age_s < HEALTH_STALE_THRESHOLD_S

    if not processes_ok:
        status = "down"
    elif not frame_fresh:
        status = "stalled"
    else:
        status = "ok"

    return {
        "status": status,
        "last_frame_age_s": round(last_frame_age_s, 2) if last_frame_age_s is not None else None,
        "stale_threshold_s": HEALTH_STALE_THRESHOLD_S,
        "daq_processes": processes,
        "daq_frame_sync": bool(web_interface.daq_frame_sync),
        "daq_iq_sync": bool(web_interface.daq_iq_sync),
        "daq_sample_delay_sync": bool(web_interface.daq_sample_delay_sync),
        "timestamp": now,
    }


_HEALTH_BADGE_STYLE = {
    "ok": {"text": "● LIVE", "className": "header_health_ok"},
    "stalled": {"text": "● STALLED", "className": "header_health_stalled"},
    "down": {"text": "● DAQ DOWN", "className": "header_health_down"},
}


def start_health_monitor(app, web_interface):
    # Started once, unconditionally, at process startup (see maindash.py) --
    # NOT from the client-connect callback like fetch_dsp_data/fetch_gps_data,
    # since a health check that only runs while a browser happens to be open
    # defeats the point of having one.
    if getattr(web_interface, "health_thread", None) is not None and web_interface.health_thread.is_alive():
        return
    web_interface.health_running = True
    web_interface.health_thread = Thread(target=_health_loop, args=(app, web_interface), daemon=True)
    web_interface.health_thread.start()


def _health_loop(app, web_interface):
    while web_interface.health_running:
        status = compute_health_status(web_interface)
        web_interface.health_status = status
        badge = _HEALTH_BADGE_STYLE[status["status"]]
        try:
            # Unlike fetch_dsp_data/fetch_gps_data (only ever started from the
            # client-connect callback, which can't fire before run_server()
            # has), this loop starts at module-import time in maindash.py --
            # so its first push_mods() calls race app.run_server() and raise
            # "Cannot call push_mods before run_server() is called." until it
            # wins that race. web_interface.health_status (read by /api/health)
            # is already updated above regardless, so just skip the UI push
            # for that brief window instead of letting it kill the thread.
            app.push_mods(
                {
                    "header_health_status": {
                        "children": badge["text"],
                        "className": badge["className"],
                    }
                }
            )
        except Exception:
            pass
        time.sleep(1)


# Wideband spectrum sweep (75-7000 MHz, channel 0 only). This talks to the
# USRP directly via UHD (see _sdr/_receiver/spectrum_sweep.py), which is
# incompatible with the normal heimdall_daq_fw acquisition chain holding the
# same hardware open -- so a sweep run stops the DAQ chain first and starts
# it again once done, same as the existing "apply DAQ config changes" flow
# (see callbacks/main.py's reconfig_daq_chain), but simpler: since a sweep
# doesn't change any DAQ config, there's no need for that flow's fragile
# in-place re-init of module_receiver/module_signal_processor -- a full
# `systemctl restart` of the service (DAQ processes + this UI process
# together) gets everything back to a known-good state exactly the way a
# normal cold restart already reliably does, dozens of times over the
# course of tonight's session.
SWEEP_RESULT_PATH = os.path.join(variables.shared_path, "sweep_result.json")


def _sweep_status_text(status):
    state = status.get("state", "idle")
    if state == "stopping_daq":
        return "Stopping DAQ chain..."
    if state == "sweeping":
        pct = status.get("progress", 0.0) * 100
        return f"Sweeping... {pct:.0f}%"
    if state == "restarting_daq":
        return "Sweep complete -- restarting DAQ chain..."
    if state == "error":
        return "Sweep failed: " + str(status.get("message", "unknown error"))
    if state == "done":
        return "Sweep complete"
    return "Idle"


def build_sweep_figure(result):
    freqs_mhz = [f / 1e6 for f in result["freqs_hz"]]
    return {
        "data": [
            {
                "x": freqs_mhz,
                "y": result["power_dbm"],
                "type": "scattergl",
                "mode": "lines",
                "line": {"color": "#00cc96", "width": 1},
                "name": "Sweep",
            }
        ],
        "layout": {
            "template": "plotly_dark",
            "xaxis": {"title": "Frequency (MHz)"},
            "yaxis": {"title": "Power (dBm, approx.)"},
            "margin": {"l": 50, "r": 20, "t": 20, "b": 40},
            "paper_bgcolor": "#000000",
            "plot_bgcolor": "#000000",
        },
    }


def start_wideband_sweep(app, web_interface):
    if getattr(web_interface, "sweep_running", False):
        web_interface.logger.debug("start_wideband_sweep: already running, ignoring request")
        return
    web_interface.sweep_running = True
    web_interface.sweep_thread = Thread(target=_sweep_worker, args=(app, web_interface), daemon=True)
    web_interface.sweep_thread.start()


def _sweep_worker(app, web_interface):
    def set_status(**kwargs):
        web_interface.sweep_status = kwargs
        try:
            app.push_mods({"sweep-status-text": {"children": _sweep_status_text(kwargs)}})
        except Exception:
            pass

    result = None
    try:
        set_status(state="stopping_daq", progress=0.0)
        # Stop the Python-side per-frame loop *before* killing the C++
        # processes it talks to -- otherwise it immediately starts spinning
        # on the same shmem-FIFO-EOF error loop diagnosed earlier tonight
        # (get_iq_online() throwing struct.error nonstop) for the whole
        # duration of the sweep.
        web_interface.stop_processing()

        prev_cwd = os.getcwd()
        os.chdir(variables.daq_subsystem_path)
        try:
            subprocess.Popen(["bash", variables.daq_stop_filename]).wait()
        finally:
            os.chdir(prev_cwd)

        # daq_stop.sh signals the processes and returns without waiting for
        # them to actually exit -- confirm the hardware is released before
        # handing it to the sweep script (up to ~10s).
        for _ in range(100):
            if not any(_daq_core_processes_alive().values()):
                break
            time.sleep(0.1)

        set_status(state="sweeping", progress=0.0)
        sweep_script = os.path.join(variables.receiver_path, "spectrum_sweep.py")
        cmd = [
            "/usr/bin/python3",
            sweep_script,
            "--daq-ini",
            variables.daq_config_filename,
            "--start-hz",
            "75e6",
            "--stop-hz",
            "7000e6",
        ]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for line in proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except ValueError:
                web_interface.logger.warning("sweep: non-JSON output from spectrum_sweep.py: %s", line)
                continue
            stage = msg.get("stage")
            if stage == "sweeping":
                set_status(state="sweeping", progress=msg.get("progress", 0.0))
            elif stage == "error":
                web_interface.logger.error("sweep failed: %s", msg.get("message"))
                set_status(state="error", progress=0.0, message=msg.get("message"))
            elif stage == "done":
                result = msg
        proc.wait()

        if result is not None:
            web_interface.sweep_result = result
            try:
                with open(SWEEP_RESULT_PATH, "w", encoding="utf-8") as f:
                    json.dump(result, f)
            except Exception:
                web_interface.logger.exception("sweep: failed to persist result to %s", SWEEP_RESULT_PATH)
            set_status(state="done", progress=1.0)
            try:
                app.push_mods({"sweep-graph": {"figure": build_sweep_figure(result)}})
            except Exception:
                pass
        elif web_interface.sweep_status.get("state") != "error":
            set_status(state="error", progress=0.0, message="sweep subprocess exited without producing a result")
    except Exception:
        web_interface.logger.exception("sweep: worker failed unexpectedly")
        set_status(state="error", progress=0.0, message="internal error, see server log")
    finally:
        set_status(state="restarting_daq", progress=1.0)
        # Fire-and-forget: this process is itself part of kraken-usrp.service,
        # so this call kills the very process making it -- do not wait() on it.
        subprocess.Popen(["systemctl", "restart", "kraken-usrp.service"])
        web_interface.sweep_running = False


def settings_change_watcher(web_interface, settings_file_path, last_attempt_failed=False):
    if os.path.exists(settings_file_path):
        last_changed_time = os.stat(settings_file_path).st_mtime
        time_delta = last_changed_time - variables.dsp_settings["timestamp"]
        if time_delta > 0:
            # Load settings file
            try:
                with open(settings_file_path, "r", encoding="utf-8") as file:
                    dsp_settings = json.load(file)
                    if dsp_settings is None:
                        raise RuntimeError("%s appears empty" % file)
            except Exception as ex:
                if not last_attempt_failed:
                    web_interface.logger.error("Problem loading settings file: %s", ex)
                last_attempt_failed = True
            else:
                variables.dsp_settings = dsp_settings
                variables.dsp_settings["timestamp"] = last_changed_time
                last_attempt_failed = False

                center_freq = float(dsp_settings.get("center_freq", 100.0))
                gain = (
                    float(dsp_settings.get("uniform_gain", 1.4))
                    if dsp_settings.get("uniform_gain", 1.4) != "Auto"
                    else AUTO_GAIN_VALUE
                )

                web_interface.en_system_control = [1] if dsp_settings.get("en_system_control", False) else []
                web_interface.en_beta_features = [1] if dsp_settings.get("en_beta_features", False) else []

                web_interface.module_signal_processor.en_DOA_estimation = dsp_settings.get("en_doa", 0)
                web_interface.module_signal_processor.DOA_decorrelation_method = dsp_settings.get(
                    "doa_decorrelation_method", 0
                )

                web_interface.module_signal_processor.DOA_ant_alignment = dsp_settings.get("ant_arrangement", "ULA")
                web_interface.ant_spacing_meters = float(dsp_settings.get("ant_spacing_meters", 0.5))

                wavelength = 300 / web_interface.daq_center_freq
                if web_interface.module_signal_processor.DOA_ant_alignment == "UCA":
                    web_interface.module_signal_processor.DOA_UCA_radius_m = web_interface.ant_spacing_meters
                    # Convert RADIUS to INTERELEMENT SPACING
                    inter_elem_spacing = (
                        np.sqrt(2)
                        * web_interface.ant_spacing_meters
                        * np.sqrt(1 - np.cos(np.deg2rad(360 / web_interface.module_signal_processor.channel_number)))
                    )
                    web_interface.module_signal_processor.DOA_inter_elem_space = inter_elem_spacing / wavelength
                else:
                    web_interface.module_signal_processor.DOA_UCA_radius_m = np.Infinity
                    web_interface.module_signal_processor.DOA_inter_elem_space = (
                        web_interface.ant_spacing_meters / wavelength
                    )

                web_interface.custom_array_x_meters = np.float_(
                    dsp_settings.get("custom_array_x_meters", "0.1,0.2,0.3,0.4,0.5").split(",")
                )
                web_interface.custom_array_y_meters = np.float_(
                    dsp_settings.get("custom_array_y_meters", "0.1,0.2,0.3,0.4,0.5").split(",")
                )
                web_interface.module_signal_processor.custom_array_x = web_interface.custom_array_x_meters / (
                    300 / web_interface.module_receiver.daq_center_freq
                )
                web_interface.module_signal_processor.custom_array_y = web_interface.custom_array_y_meters / (
                    300 / web_interface.module_receiver.daq_center_freq
                )

                # Station Information
                web_interface.module_signal_processor.station_id = dsp_settings.get("station_id", "NO-CALL")
                web_interface.location_source = dsp_settings.get("location_source", "None")
                web_interface.module_signal_processor.latitude = dsp_settings.get("latitude", 0.0)
                web_interface.module_signal_processor.longitude = dsp_settings.get("longitude", 0.0)
                web_interface.module_signal_processor.heading = dsp_settings.get("heading", 0.0)
                web_interface.module_signal_processor.krakenpro_key = dsp_settings.get("krakenpro_key", 0.0)
                web_interface.mapping_server_url = dsp_settings.get(
                    "mapping_server_url", DEFAULT_MAPPING_SERVER_ENDPOINT
                )
                web_interface.module_signal_processor.RDF_mapper_server = dsp_settings.get(
                    "rdf_mapper_server", "http://RDF_MAPPER_SERVER.com/save.php"
                )
                web_interface.module_signal_processor.DOA_data_format = dsp_settings.get(
                    "doa_data_format", "Kraken App"
                )

                # VFO Configuration
                web_interface.module_signal_processor.spectrum_fig_type = dsp_settings.get(
                    "spectrum_calculation", "Single"
                )
                web_interface.module_signal_processor.vfo_mode = dsp_settings.get("vfo_mode", "Standard")
                web_interface.module_signal_processor.vfo_default_squelch_mode = dsp_settings.get(
                    "vfo_default_squelch_mode", "Auto"
                )
                web_interface.module_signal_processor.vfo_default_demod = dsp_settings.get("vfo_default_demod", "None")
                web_interface.module_signal_processor.vfo_default_iq = dsp_settings.get("vfo_default_iq", "False")
                web_interface.module_signal_processor.max_demod_timeout = int(dsp_settings.get("max_demod_timeout", 60))
                web_interface.module_signal_processor.dsp_decimation = int(dsp_settings.get("dsp_decimation", 0))
                web_interface.module_signal_processor.active_vfos = int(dsp_settings.get("active_vfos", 0))
                web_interface.module_signal_processor.output_vfo = int(dsp_settings.get("output_vfo", 0))
                web_interface.compass_offset = dsp_settings.get("compass_offset", 0)
                web_interface.module_signal_processor.compass_offset = web_interface.compass_offset
                web_interface.module_signal_processor.optimize_short_bursts = dsp_settings.get(
                    "en_optimize_short_bursts", 0
                )
                web_interface.module_signal_processor.en_peak_hold = dsp_settings.get("en_peak_hold", 0)

                for i in range(web_interface.module_signal_processor.max_vfos):
                    web_interface.module_signal_processor.vfo_bw[i] = int(dsp_settings.get("vfo_bw_" + str(i), 0))
                    web_interface.module_signal_processor.vfo_fir_order_factor[i] = int(
                        dsp_settings.get("vfo_fir_order_factor_" + str(i), DEFAULT_VFO_FIR_ORDER_FACTOR)
                    )
                    web_interface.module_signal_processor.vfo_freq[i] = float(dsp_settings.get("vfo_freq_" + str(i), 0))
                    web_interface.module_signal_processor.vfo_squelch_mode[i] = dsp_settings.get(
                        "vfo_squelch_mode_" + str(i), "Default"
                    )
                    web_interface.module_signal_processor.vfo_squelch[i] = int(
                        dsp_settings.get("vfo_squelch_" + str(i), 0)
                    )
                    web_interface.module_signal_processor.vfo_demod[i] = dsp_settings.get(
                        "vfo_demod_" + str(i), "Default"
                    )
                    web_interface.module_signal_processor.vfo_iq[i] = dsp_settings.get("vfo_iq_" + str(i), "Default")

                web_interface.module_signal_processor.DOA_algorithm = dsp_settings.get("doa_method", "MUSIC")
                web_interface.module_signal_processor.DOA_expected_num_of_sources = dsp_settings.get(
                    "expected_num_of_sources", 1
                )
                web_interface._doa_fig_type = dsp_settings.get("doa_fig_type", "Linear")
                web_interface.module_signal_processor.doa_measure = web_interface._doa_fig_type
                web_interface.module_signal_processor.ula_direction = dsp_settings.get("ula_direction", "Both")
                web_interface.module_signal_processor.array_offset = int(dsp_settings.get("array_offset", 0))

                freq_delta = web_interface.daq_center_freq - center_freq
                gain_delta = web_interface.module_receiver.daq_rx_gain - gain

                if abs(freq_delta) > 0.001 or abs(gain_delta) > 0.001:
                    web_interface.daq_center_freq = center_freq
                    web_interface.config_daq_rf(center_freq, gain)
                    for i in range(web_interface.module_signal_processor.max_vfos):
                        half_band_width = (web_interface.module_signal_processor.vfo_bw[i] / 10**6) / 2
                        min_freq = web_interface.daq_center_freq - web_interface.daq_fs / 2 + half_band_width
                        max_freq = web_interface.daq_center_freq + web_interface.daq_fs / 2 - half_band_width
                        if min_freq > (web_interface.module_signal_processor.vfo_freq[i] / 10**6) or max_freq < (
                            web_interface.module_signal_processor.vfo_freq[i] / 10**6
                        ):
                            web_interface.module_signal_processor.vfo_freq[i] = (
                                web_interface.module_receiver.daq_center_freq
                            )

                    wavelength = 300 / web_interface.daq_center_freq

                    if web_interface.module_signal_processor.DOA_ant_alignment == "UCA":
                        # Convert RADIUS to INTERELEMENT SPACING
                        inter_elem_spacing = (
                            np.sqrt(2)
                            * web_interface.ant_spacing_meters
                            * np.sqrt(
                                1 - np.cos(np.deg2rad(360 / web_interface.module_signal_processor.channel_number))
                            )
                        )
                        web_interface.module_signal_processor.DOA_inter_elem_space = inter_elem_spacing / wavelength
                    else:
                        web_interface.module_signal_processor.DOA_inter_elem_space = (
                            web_interface.ant_spacing_meters / wavelength
                        )

                if dsp_settings.get("ext_upd_flag", False):
                    web_interface.needs_refresh = True
                    web_interface.save_configuration()

    web_interface.settings_change_timer = Timer(
        0.5, settings_change_watcher, args=(web_interface, settings_file_path, last_attempt_failed)
    )
    # FIX (slow service stop): this chain reschedules itself forever (every
    # 0.5s, for the life of the process) and Timer defaults to non-daemon --
    # so there is *always* a live non-daemon Timer thread, and each one
    # spawns its own non-daemon replacement just before it exits. CPython's
    # interpreter shutdown (threading._shutdown()) waits for every non-daemon
    # thread to finish, and since a fresh one keeps appearing right as the
    # last one dies, that wait never naturally ends -- the process hangs
    # until systemd's TimeoutStopSec expires and SIGKILLs it (observed
    # tonight: consistently ~90s on every single restart of this service,
    # not just ones involving the wideband sweep feature). Marking it daemon
    # is enough on its own: unlike fetch_dsp_data/fetch_gps_data's old Timer
    # chain (which needed the full persistent-thread rewrite to stop
    # accumulating duplicate chains across reconnects), this one was never
    # duplicating -- it just needed to stop blocking shutdown.
    web_interface.settings_change_timer.daemon = True
    web_interface.settings_change_timer.start()


def update_daq_status(app, web_interface):
    #############################################
    #      Prepare UI component properties      #
    #############################################

    if web_interface.daq_frame_type == "Empty":

        daq_frame_type_str = "Empty (likely failure)"
        frame_type_style = RED_COLOR

        daq_conn_status_str = "Unknown (likely failure)"
        conn_status_style = RED_COLOR

        daq_frame_sync_str = "-"
        frame_sync_style = RED_COLOR

        daq_delay_sync_str = "-"
        delay_sync_style = RED_COLOR

        daq_iq_sync_str = "-"
        iq_sync_style = RED_COLOR

        daq_power_level_str = "-"
        daq_power_level_style = RED_COLOR

        daq_noise_source_str = "-"
        noise_source_style = RED_COLOR

        daq_frame_index_str = "-"
        daq_dsp_latency = "-"
        daq_update_rate_str = "-"

        daq_rf_center_freq_str = "-"
        daq_sampling_freq_str = "-"
        dsp_decimated_bw_str = "-"
        vfo_range_str = "-"

        daq_cpi_str = "-"
        daq_max_amp_str = "-"
        daq_avg_powers_str = "-"
    else:
        if web_interface.daq_conn_status == 1:
            if not web_interface.daq_cfg_iface_status:
                daq_conn_status_str = "Connected"
                conn_status_style = {"color": "#7ccc63"}
            else:  # Config interface is busy
                daq_conn_status_str = "Reconfiguration.."
                conn_status_style = {"color": "#f39c12"}
        else:
            daq_conn_status_str = "Disconnected"
            conn_status_style = {"color": "#e74c3c"}

        if web_interface.daq_restart:
            daq_conn_status_str = "Restarting.."
            conn_status_style = {"color": "#f39c12"}

        if web_interface.daq_update_rate < 1:
            daq_update_rate_str = "{:d} ms".format(round(web_interface.daq_update_rate * 1000))
        else:
            daq_update_rate_str = "{:.2f} s".format(web_interface.daq_update_rate)

        daq_dsp_latency = "{:d} ms".format(web_interface.daq_dsp_latency)
        daq_frame_index_str = str(web_interface.daq_frame_index)

        daq_frame_type_str = web_interface.daq_frame_type
        if web_interface.daq_frame_type == "Data":
            frame_type_style = frame_type_style = {"color": "#7ccc63"}
        elif web_interface.daq_frame_type == "Dummy":
            frame_type_style = frame_type_style = {"color": "white"}
        elif web_interface.daq_frame_type == "Calibration":
            frame_type_style = frame_type_style = {"color": "#f39c12"}
        elif web_interface.daq_frame_type == "Trigger wait":
            frame_type_style = frame_type_style = {"color": "#f39c12"}
        else:
            frame_type_style = frame_type_style = {"color": "#e74c3c"}

        if web_interface.daq_frame_sync:
            daq_frame_sync_str = "LOSS"
            frame_sync_style = {"color": "#e74c3c"}
        else:
            daq_frame_sync_str = "Ok"
            frame_sync_style = {"color": "#7ccc63"}
        if web_interface.daq_sample_delay_sync:
            daq_delay_sync_str = "Ok"
            delay_sync_style = {"color": "#7ccc63"}
        else:
            daq_delay_sync_str = "LOSS"
            delay_sync_style = {"color": "#e74c3c"}

        if web_interface.daq_iq_sync:
            daq_iq_sync_str = "Ok"
            iq_sync_style = {"color": "#7ccc63"}
        else:
            daq_iq_sync_str = "LOSS"
            iq_sync_style = {"color": "#e74c3c"}

        if web_interface.daq_noise_source_state:
            daq_noise_source_str = "Enabled"
            noise_source_style = {"color": "#e74c3c"}
        else:
            daq_noise_source_str = "Disabled"
            noise_source_style = {"color": "#7ccc63"}

        if web_interface.daq_power_level:
            daq_power_level_str = "Overdrive"
            daq_power_level_style = {"color": "#e74c3c"}
        else:
            daq_power_level_str = "OK"
            daq_power_level_style = {"color": "#7ccc63"}

        daq_rf_center_freq_str = str(web_interface.daq_center_freq)
        daq_sampling_freq_str = str(web_interface.daq_fs)
        bw = web_interface.daq_fs / web_interface.module_signal_processor.dsp_decimation
        dsp_decimated_bw_str = "{0:.3f}".format(bw)
        vfo_range_str = (
            "{0:.3f}".format(web_interface.daq_center_freq - bw / 2)
            + " - "
            + "{0:.3f}".format(web_interface.daq_center_freq + bw / 2)
        )
        daq_cpi_str = str(web_interface.daq_cpi)
        daq_max_amp_str = "{:.1f}".format(web_interface.max_amplitude)
        daq_avg_powers_str = web_interface.avg_powers

    if web_interface.module_signal_processor.gps_status == "Connected":
        gps_en_str = "Connected"
        gps_en_str_style = {"color": "#7ccc63"}
    else:
        gps_en_str = web_interface.module_signal_processor.gps_status
        gps_en_str_style = {"color": "#e74c3c"}

    app.push_mods(
        {
            "body_daq_update_rate": {"children": daq_update_rate_str},
            "body_daq_dsp_latency": {"children": daq_dsp_latency},
            "body_daq_frame_index": {"children": daq_frame_index_str},
            "body_daq_frame_sync": {"children": daq_frame_sync_str},
            "body_daq_frame_type": {"children": daq_frame_type_str},
            "body_daq_power_level": {"children": daq_power_level_str},
            "body_daq_conn_status": {"children": daq_conn_status_str},
            "body_daq_delay_sync": {"children": daq_delay_sync_str},
            "body_daq_iq_sync": {"children": daq_iq_sync_str},
            "body_daq_noise_source": {"children": daq_noise_source_str},
            "body_daq_rf_center_freq": {"children": daq_rf_center_freq_str},
            "body_daq_sampling_freq": {"children": daq_sampling_freq_str},
            "body_dsp_decimated_bw": {"children": dsp_decimated_bw_str},
            "body_vfo_range": {"children": vfo_range_str},
            "body_daq_cpi": {"children": daq_cpi_str},
            "body_daq_if_gain": {"children": web_interface.daq_if_gains},
            "body_max_amp": {"children": daq_max_amp_str},
            "body_avg_powers": {"children": daq_avg_powers_str},
            "gps_status": {"children": gps_en_str},
        }
    )

    app.push_mods(
        {
            "body_daq_frame_sync": {"style": frame_sync_style},
            "body_daq_frame_type": {"style": frame_type_style},
            "body_daq_power_level": {"style": daq_power_level_style},
            "body_daq_conn_status": {"style": conn_status_style},
            "body_daq_delay_sync": {"style": delay_sync_style},
            "body_daq_iq_sync": {"style": iq_sync_style},
            "body_daq_noise_source": {"style": noise_source_style},
            "gps_status": {"style": gps_en_str_style},
        }
    )

    # Update local recording file size
    recording_file_size = web_interface.module_signal_processor.get_recording_filesize()
    app.push_mods({"body_file_size": {"children": recording_file_size}})


def get_agc_warning_style_from_gain(gain):
    return (
        copy.deepcopy(AGC_WARNING_ENABLED_STYLE)
        if gain == AUTO_GAIN_VALUE
        else copy.deepcopy(AGC_WARNING_DISABLED_STYLE)
    )


"""
Input validation utilities
"""


def is_float(string, minimum=-inf, maximum=inf):
    try:
        f = float(string)
        return f >= minimum and f <= maximum
    except (ValueError, TypeError):
        return False


def is_int(string, minimum=-inf, maximum=inf):
    try:
        i = int(string)
        return i >= minimum and i <= maximum
    except (ValueError, TypeError):
        return False
