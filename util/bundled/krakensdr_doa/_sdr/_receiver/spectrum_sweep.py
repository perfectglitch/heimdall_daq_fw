"""
Standalone wideband spectrum sweep, run as a subprocess under the SYSTEM
python3 (NOT the app's conda env) -- python3-uhd is only installed there.

This talks to the USRP directly via UHD, bypassing the whole heimdall_daq_fw
acquisition chain (usrp_daq.out/decimate.out/rebuffer.out/delay_sync.py/
hw_controller.py). UHD does not allow two processes to open the same USRP at
once, so the caller MUST have those five processes stopped (see
utils.start_wideband_sweep, which runs daq_stop.sh first) before launching
this script, and is responsible for starting the DAQ chain back up once this
exits.

Emits one JSON object per line on stdout: progress updates while sweeping,
and a final result (or error) object. Nothing else goes to stdout, so the
caller can safely json.loads() every line.
"""
import argparse
import configparser
import json
import os
import sys
import time

import numpy as np
import uhd
from scipy import signal as scipy_signal

# FIX (nonsensical scale): the original version computed 10*log10(|fft(x)|^2)
# from numpy's raw, UNNORMALIZED FFT -- magnitude scales with fft_size, so
# absolute level was off by 20*log10(fft_size) (~+78 dB at fft_size=8192) and
# depended on fft_size/window choice instead of the actual signal. Replaced
# with scipy.signal.welch(..., scaling="spectrum"), matching the normalization
# the existing per-channel live spectrum already uses (see kraken_sdr_signal_
# processor.py's own welch() call) -- same convention as the view that was
# never reported as "not making sense".
#
# That gives a properly-scaled dBFS (relative to ADC full scale), not dBm --
# UHD/the B210 provide no absolute power calibration, so true dBm needs
# calibrating against a known signal generator, which isn't available here.
# REF_DBM_AT_0DB_GAIN is a commonly-cited rough figure for AD9361-based radios
# (the B210's RF chip) for the RF input power that drives the ADC to full
# scale at 0 dB gain; approximate, not a lab calibration -- treat absolute
# levels as +/- several dB, though relative levels (comparing two peaks in the
# same sweep) are trustworthy since they share the same reference and gain.
REF_DBM_AT_0DB_GAIN = -10.0


def load_channel0_device_args(daq_ini_path):
    parser = configparser.ConfigParser()
    parser.read(daq_ini_path)
    serial = parser.get("usrp", "serial_0")
    fpga_path = parser.get("usrp", "fpga_path", fallback="")
    fw_path = parser.get("usrp", "fw_path", fallback="")
    # fpga_path/fw_path in the ini are relative to the ini file's own
    # directory (see build_addr() in usrp_daq.cc, which resolves them the
    # same way relative to its cwd) -- resolve to absolute so this script's
    # own cwd doesn't matter.
    ini_dir = os.path.dirname(os.path.abspath(daq_ini_path))
    args = f"serial={serial}"
    if fpga_path:
        args += f",fpga={os.path.abspath(os.path.join(ini_dir, fpga_path))}"
    if fw_path:
        args += f",fw={os.path.abspath(os.path.join(ini_dir, fw_path))}"
    return args


def emit(obj):
    print(json.dumps(obj), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--daq-ini", required=True)
    ap.add_argument("--start-hz", type=float, default=75e6)
    ap.add_argument("--stop-hz", type=float, default=7000e6)
    ap.add_argument("--sample-rate", type=float, default=20e6)
    ap.add_argument("--gain-db", type=float, default=40.0)
    ap.add_argument("--fft-size", type=int, default=8192)
    # Fraction of each capture's bandwidth kept per step (the rest is
    # discarded to avoid the decimation/anti-alias filter roll-off near the
    # edges of the passband) -- also doubles as the step size as a fraction
    # of sample_rate, so steps tile the range with no gaps.
    ap.add_argument("--usable-frac", type=float, default=0.8)
    ap.add_argument("--output-points", type=int, default=8192)
    args = ap.parse_args()

    try:
        device_args = load_channel0_device_args(args.daq_ini)
    except Exception as e:
        emit({"stage": "error", "message": f"failed to read {args.daq_ini}: {e}"})
        sys.exit(1)

    emit({"stage": "opening_device", "device_args": device_args})

    try:
        usrp = uhd.usrp.MultiUSRP(device_args)
        usrp.set_rx_rate(args.sample_rate, 0)
        usrp.set_rx_bandwidth(args.sample_rate, 0)
        usrp.set_rx_gain(args.gain_db, 0)
    except Exception as e:
        emit({"stage": "error", "message": f"failed to open/configure USRP: {e}"})
        sys.exit(1)

    freq_range = usrp.get_fe_rx_freq_range(0)
    hw_min_hz = freq_range.start()
    hw_max_hz = freq_range.stop()

    sweep_start_hz = max(args.start_hz, hw_min_hz)
    sweep_stop_hz = min(args.stop_hz, hw_max_hz)
    if sweep_stop_hz <= sweep_start_hz:
        emit(
            {
                "stage": "error",
                "message": (
                    f"requested range {args.start_hz}-{args.stop_hz} Hz does not overlap "
                    f"this radio's tunable range {hw_min_hz}-{hw_max_hz} Hz"
                ),
            }
        )
        sys.exit(1)

    step_hz = args.sample_rate * args.usable_frac
    usable_half_hz = step_hz / 2.0

    n_steps = int(np.ceil((sweep_stop_hz - sweep_start_hz) / step_hz)) + 1
    center_freqs = sweep_start_hz + usable_half_hz + np.arange(n_steps) * step_hz
    center_freqs = center_freqs[center_freqs - usable_half_hz < sweep_stop_hz]

    st_args = uhd.usrp.StreamArgs("fc32", "sc16")
    st_args.channels = [0]
    try:
        rx_streamer = usrp.get_rx_stream(st_args)
    except Exception as e:
        emit({"stage": "error", "message": f"failed to create rx streamer: {e}"})
        sys.exit(1)

    settle_s = 0.05
    n_samps = args.fft_size * 4  # a few FFT windows per step, welch() averages them for less noisy PSD

    all_freqs = []
    all_power_dbm = []
    metadata = uhd.types.RXMetadata()

    emit({"stage": "sweeping", "progress": 0.0, "n_steps": len(center_freqs)})

    for i, fc in enumerate(center_freqs):
        usrp.set_rx_freq(uhd.types.TuneRequest(float(fc)), 0)
        time.sleep(settle_s)

        stream_cmd = uhd.types.StreamCMD(uhd.types.StreamMode.num_done)
        stream_cmd.num_samps = n_samps
        stream_cmd.stream_now = True
        rx_streamer.issue_stream_cmd(stream_cmd)

        chunk = np.zeros((1, n_samps), dtype=np.complex64)
        recvd = 0
        while recvd < n_samps:
            n = rx_streamer.recv(chunk[:, recvd:], metadata, 1.0)
            if n == 0 or metadata.error_code != uhd.types.RXMetadataErrorCode.none:
                break
            recvd += n

        if recvd < args.fft_size:
            # Bad/short capture at this step -- skip it rather than poison
            # the plot with a zero-filled FFT; a gap in the trace is more
            # honest than a fake noise floor at this frequency.
            emit({"stage": "sweeping", "progress": (i + 1) / len(center_freqs), "freq_hz": fc, "step_failed": True})
            continue

        iq = chunk[0, :recvd]
        freqs_bin, psd = scipy_signal.welch(
            iq,
            args.sample_rate,
            nperseg=args.fft_size,
            nfft=args.fft_size,
            noverlap=0,
            detrend=False,
            return_onesided=False,
            window="blackman",
            scaling="spectrum",
        )
        psd_dbfs = np.fft.fftshift(10 * np.log10(psd + 1e-20))
        psd_dbm = psd_dbfs - args.gain_db + REF_DBM_AT_0DB_GAIN

        bin_freqs = fc + np.fft.fftshift(freqs_bin)
        keep = np.abs(bin_freqs - fc) <= usable_half_hz
        all_freqs.append(bin_freqs[keep])
        all_power_dbm.append(psd_dbm[keep])

        emit({"stage": "sweeping", "progress": (i + 1) / len(center_freqs), "freq_hz": fc})

    if not all_freqs:
        emit({"stage": "error", "message": "no usable captures across the whole sweep"})
        sys.exit(1)

    freqs_hz = np.concatenate(all_freqs)
    power_dbm = np.concatenate(all_power_dbm)
    order = np.argsort(freqs_hz)
    freqs_hz = freqs_hz[order]
    power_dbm = power_dbm[order]

    # Full resolution here is one FFT bin per ~2.4 kHz across ~6 GHz -- millions
    # of points, a ~90 MB JSON blob nobody's browser or API client needs.
    # Max-hold decimate down to a UI/API-friendly point count (max, not mean,
    # so a narrow peak inside a bin doesn't get averaged away).
    if len(freqs_hz) > args.output_points:
        edges = np.linspace(0, len(freqs_hz), args.output_points + 1).astype(int)
        freqs_hz = np.array([freqs_hz[edges[i] : edges[i + 1]].mean() for i in range(args.output_points)])
        power_dbm = np.array([power_dbm[edges[i] : edges[i + 1]].max() for i in range(args.output_points)])

    emit(
        {
            "stage": "done",
            "requested_start_hz": args.start_hz,
            "requested_stop_hz": args.stop_hz,
            "hw_min_hz": hw_min_hz,
            "hw_max_hz": hw_max_hz,
            "actual_start_hz": float(freqs_hz[0]),
            "actual_stop_hz": float(freqs_hz[-1]),
            "sample_rate_hz": args.sample_rate,
            "gain_db": args.gain_db,
            "power_reference": "approximate dBm -- not lab-calibrated, see REF_DBM_AT_0DB_GAIN",
            "freqs_hz": freqs_hz.tolist(),
            "power_dbm": power_dbm.tolist(),
            "timestamp": time.time(),
        }
    )


if __name__ == "__main__":
    main()
