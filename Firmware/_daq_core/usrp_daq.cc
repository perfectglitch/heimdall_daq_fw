/*
 *
 * Description :
 * Coherent multichannel receiver for 3x USRP B210-class devices (2+2+1
 * channels), with an integrated HackRF-driven broadband noise source used
 * for amplitude/phase calibration in place of the RTL-SDR bias-tee diode.
 *
 * This is the USRP analogue of rtl_daq.c: a standalone daemon that writes
 * iq_header_struct + raw IQ payload frames to stdout and exposes the same
 * ZMQ REP control socket on port 1130 (hdaq_im_msg_struct commands
 * 'r','c','g','a','s','n','h') used by hw_controller.py / delay_sync.py.
 * Unlike rtl_daq.c, samples are carried as signed 16-bit I/Q
 * (sample_bit_depth=16) instead of RTL-SDR's native unsigned 8-bit.
 *
 * Project : HeIMDALL DAQ Firmware
 * License : GNU GPL V3
 *
 * Copyright 2026 en7
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/time.h>
#include <thread>
#include <vector>

#include <zmq.h>

#include <uhd/exception.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/thread.hpp>

#include <libhackrf/hackrf.h>

extern "C" {
#include "log.h"
#include "iq_header.h"
}
#include "ini.h"
#include "im_msg.h"

#define INI_FNAME "daq_chain_config.ini"
#define MAX_USRP 3
#define NUM_BUFF 32              // Number of buffers used in the circular, coherent read buffer
                                  // (~3.5s of cushion at the default daq_buffer_size/sample_rate --
                                  // see the main loop's lock-scope comment for why this needs to be
                                  // generous: it's what absorbs a slow/stalled downstream consumer
                                  // without that backpressure reaching the USRP reader threads)
#define RECV_TIMEOUT_S 0.5       // UHD recv() timeout, also bounds how fast 'h' halt is noticed
#define OVERDRIVE_THRESHOLD 32760
#define NO_DUMMY_FRAMES 5
#define FALLBACK_ANTENNA "RX2"

/*
 * ------> DUMMY FRAMES <------
 * If enabled, the module continues the acquisition, but sends out dummy
 * frames only, until NO_DUMMY_FRAMES number of frames have not been sent out.
 * (Mirrors rtl_daq.c's behavior while a reconfiguration takes effect.)
 */
static int en_dummy_frame = 0;
static int dummy_frame_cntr = 0;

/*
 * This structure stores the configuration parameters loaded from the ini file
 */
typedef struct
{
    // [hw]
    int num_ch;
    const char* hw_name;
    int hw_unit_id;
    int ioo_type;
    // [daq]
    int daq_buffer_size;
    long sample_rate;
    long center_freq;
    long lo_offset;         // Hz, 0 = disabled. B2x0 is zero-IF, so LO leakage
                             // lands as a DC spike right at center_freq; tuning
                             // the LO this far away (RF) while UHD's DSP mixer
                             // shifts back to center_freq (baseband) moves that
                             // spike out of the band of interest instead.
    int gain;               // tenths of dB, e.g. 300 = 30.0 dB
    int en_noise_source_ctr;
    int log_level;
    // [calibration]
    int std_ch_ind;         // Must mirror delay_sync.py's [calibration] std_ch_ind --
                             // that module measures every channel's delay against this
                             // one, so the 's' handler's reference-device skip target
                             // (see its comment) has to agree on which flat channel is
                             // "the standard channel" or its corrections target the
                             // wrong device pairing.
    // [usrp]
    char serial[MAX_USRP][64];
    int num_ch_dev[MAX_USRP];
    char subdev[32];
    char antenna[16];
    int pps_settle_ms;
    char fpga_path[256];
    char fw_path[256];
    // Per-device fine timing trim: PPS/set_time_next_pps aligns each USRP's
    // time=0 reference, but a device can still have a small, fixed (not
    // drifting) sample-level latency relative to the others -- e.g. from
    // FPGA pipeline depth differences. delay_sync.py's STATE_SAMPLE_CAL only
    // has a ppm *drift* trim available (the 's' command, a no-op here since
    // USRP's clock is PPS-shared, not free-running), so it can never correct
    // a constant offset -- this per-slot knob (positive = start streaming
    // this many ns earlier) exists for that one-time hardware trim instead.
    // Measure via delay_sync.py's STATE_SAMPLE_CAL debug log ("Channel N,
    // delay: ..."), convert samples -> ns (delay/sample_rate*1e9), and adjust
    // here; 0 (default) is a no-op.
    long stream_start_offset_ns[MAX_USRP];
    // [noise_source]
    char hackrf_serial[64];
    int hackrf_tx_sample_rate;
    float hackrf_tx_gain_db;
    int hackrf_amp_enable;
    long hackrf_tx_freq;    // 0 = auto-track center_freq
    double hackrf_freq_correction_ppm; // see HackRFNoiseSource::freq_correction_ppm
} configuration;

static int handler(void* conf_struct, const char* section, const char* name, const char* value)
{
    configuration* pconfig = (configuration*) conf_struct;

    #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
    if (MATCH("hw", "num_ch"))              {pconfig->num_ch = atoi(value);}
    else if (MATCH("hw", "name"))           {pconfig->hw_name = strdup(value);}
    else if (MATCH("hw", "unit_id"))        {pconfig->hw_unit_id = atoi(value);}
    else if (MATCH("hw", "ioo_type"))       {pconfig->ioo_type = atoi(value);}
    else if (MATCH("daq", "daq_buffer_size")) {pconfig->daq_buffer_size = atoi(value);}
    else if (MATCH("daq", "sample_rate"))   {pconfig->sample_rate = atol(value);}
    else if (MATCH("daq", "center_freq"))   {pconfig->center_freq = atol(value);}
    else if (MATCH("daq", "lo_offset"))     {pconfig->lo_offset = atol(value);}
    else if (MATCH("daq", "gain"))          {pconfig->gain = atoi(value);}
    else if (MATCH("daq", "en_noise_source_ctr")) {pconfig->en_noise_source_ctr = atoi(value);}
    else if (MATCH("daq", "log_level"))     {pconfig->log_level = atoi(value);}
    else if (MATCH("calibration", "std_ch_ind")) {pconfig->std_ch_ind = atoi(value);}
    else if (MATCH("usrp", "serial_0"))     {strncpy(pconfig->serial[0], value, sizeof(pconfig->serial[0])-1);}
    else if (MATCH("usrp", "serial_1"))     {strncpy(pconfig->serial[1], value, sizeof(pconfig->serial[1])-1);}
    else if (MATCH("usrp", "serial_2"))     {strncpy(pconfig->serial[2], value, sizeof(pconfig->serial[2])-1);}
    else if (MATCH("usrp", "num_ch_0"))     {pconfig->num_ch_dev[0] = atoi(value);}
    else if (MATCH("usrp", "num_ch_1"))     {pconfig->num_ch_dev[1] = atoi(value);}
    else if (MATCH("usrp", "num_ch_2"))     {pconfig->num_ch_dev[2] = atoi(value);}
    else if (MATCH("usrp", "subdev"))       {strncpy(pconfig->subdev, value, sizeof(pconfig->subdev)-1);}
    else if (MATCH("usrp", "antenna"))      {strncpy(pconfig->antenna, value, sizeof(pconfig->antenna)-1);}
    else if (MATCH("usrp", "pps_settle_ms")) {pconfig->pps_settle_ms = atoi(value);}
    else if (MATCH("usrp", "fpga_path"))    {strncpy(pconfig->fpga_path, value, sizeof(pconfig->fpga_path)-1);}
    else if (MATCH("usrp", "fw_path"))      {strncpy(pconfig->fw_path, value, sizeof(pconfig->fw_path)-1);}
    else if (MATCH("usrp", "stream_start_offset_ns_0")) {pconfig->stream_start_offset_ns[0] = atol(value);}
    else if (MATCH("usrp", "stream_start_offset_ns_1")) {pconfig->stream_start_offset_ns[1] = atol(value);}
    else if (MATCH("usrp", "stream_start_offset_ns_2")) {pconfig->stream_start_offset_ns[2] = atol(value);}
    else if (MATCH("noise_source", "hackrf_serial")) {strncpy(pconfig->hackrf_serial, value, sizeof(pconfig->hackrf_serial)-1);}
    else if (MATCH("noise_source", "tx_sample_rate")) {pconfig->hackrf_tx_sample_rate = atoi(value);}
    else if (MATCH("noise_source", "tx_gain_db")) {pconfig->hackrf_tx_gain_db = (float) atof(value);}
    else if (MATCH("noise_source", "amp_enable")) {pconfig->hackrf_amp_enable = atoi(value);}
    else if (MATCH("noise_source", "tx_freq")) {pconfig->hackrf_tx_freq = atol(value);}
    else if (MATCH("noise_source", "freq_correction_ppm")) {pconfig->hackrf_freq_correction_ppm = atof(value);}
    else {return 0;}
    return 0;
}

/*
 *===========================================================================
 *  HackRF-driven noise source (replaces RTL-SDR's bias-tee noise diode)
 *===========================================================================
 * A calibration-grade broadband signal is synthesized on the fly (fast
 * xorshift PRNG -> full-scale int8 I/Q) directly inside the HackRF TX
 * callback -- no producer thread/ring buffer is needed since the "source"
 * data doesn't come from anywhere else, unlike Firmware/usrp/hackrf_noise_tx_impl.cc
 * (which accepts an external GNU Radio sample stream).
 *
 * The external RF switch is automatic hardware tied to HackRF TX activity:
 * turning TX on/off is the entire noise-source control surface, there is no
 * software GPIO/antenna-switch step.
 */
struct HackRFNoiseSource
{
    hackrf_device* dev = nullptr;
    bool available = false;
    std::atomic<bool> tx_active{false};
    uint32_t rng_state = 0x9E3779B9u;

    long tx_freq_override = 0; // 0 = auto-track RX center_freq
    long current_freq = 0;
    // Non-original/clone HackRF units can have a crystal well outside the
    // genuine board's ~20ppm spec -- libhackrf has no equivalent of
    // rtlsdr_set_freq_correction to calibrate this out, so it has to be
    // compensated here instead. The error is proportional to absolute
    // frequency, which is why this can look fine at low frequencies (a few
    // hundred kHz off at 900MHz, lost in the 8MHz TX bandwidth) and only
    // become a real problem higher up (up to ~2MHz off at 5.8GHz, enough to
    // partially or fully miss the USRP's much narrower RX capture window --
    // see daq_chain_config.ini's sample_rate). 0 = no correction (previous
    // behavior).
    double freq_correction_ppm = 0.0;

    static uint32_t xorshift32(uint32_t& s)
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }

    static int tx_callback_tramp(hackrf_transfer* transfer)
    {
        return reinterpret_cast<HackRFNoiseSource*>(transfer->tx_ctx)->fill(transfer);
    }

    int fill(hackrf_transfer* transfer)
    {
        int8_t* dst = reinterpret_cast<int8_t*>(transfer->buffer);
        int n = transfer->valid_length;
        for (int i = 0; i < n; i++)
        {
            // Full-scale-ish white noise, clamped away from -128 to keep I/Q symmetric
            int8_t v = (int8_t)((int32_t)(xorshift32(rng_state) & 0xFF) - 128);
            if (v == -128) v = -127;
            dst[i] = v;
        }
        return 0;
    }

    bool init(const char* serial, int sample_rate, float gain_db, int amp_enable, long tx_freq, double freq_correction_ppm_)
    {
        tx_freq_override = tx_freq;
        freq_correction_ppm = freq_correction_ppm_;
        int rc = hackrf_init();
        if (rc != HACKRF_SUCCESS)
        {
            log_error("hackrf_init failed: %s", hackrf_error_name((hackrf_error) rc));
            return false;
        }
        if (serial != NULL && strlen(serial) > 0)
            rc = hackrf_open_by_serial(serial, &dev);
        else
            rc = hackrf_open(&dev);
        if (rc != HACKRF_SUCCESS)
        {
            log_error("Failed to open HackRF noise source: %s", hackrf_error_name((hackrf_error) rc));
            dev = NULL;
            return false;
        }
        hackrf_set_sample_rate(dev, sample_rate);
        uint32_t vga = (uint32_t) (gain_db < 0 ? 0 : (gain_db > 47 ? 47 : gain_db));
        hackrf_set_txvga_gain(dev, vga);
        hackrf_set_amp_enable(dev, amp_enable ? 1 : 0);
        available = true;
        log_info("HackRF noise source initialized");
        return true;
    }

    void set_center_freq(long rx_center_freq)
    {
        if (!available) return;
        long target_freq = (tx_freq_override != 0) ? tx_freq_override : rx_center_freq;
        // Positive ppm = this unit's crystal runs fast, so it actually
        // transmits *above* whatever we ask for -- ask for less to
        // compensate, and vice versa for a slow crystal.
        current_freq = target_freq - (long) std::llround(target_freq * (freq_correction_ppm / 1.0e6));
        hackrf_set_freq(dev, (uint64_t) current_freq);
        log_info("HackRF noise source tuned to %ld Hz (requested %ld Hz, correction %.1f ppm)",
                 current_freq, target_freq, freq_correction_ppm);
    }

    void set_enabled(bool enable)
    {
        if (!available) return;
        if (enable && !tx_active.load())
        {
            hackrf_set_freq(dev, (uint64_t) current_freq);
            int rc = hackrf_start_tx(dev, tx_callback_tramp, this);
            if (rc != HACKRF_SUCCESS)
                log_error("hackrf_start_tx failed: %s", hackrf_error_name((hackrf_error) rc));
            else
                tx_active.store(true);
        }
        else if (!enable && tx_active.load())
        {
            hackrf_stop_tx(dev);
            tx_active.store(false);
        }
    }

    void close()
    {
        if (dev != NULL)
        {
            hackrf_stop_tx(dev);
            hackrf_close(dev);
            dev = NULL;
        }
        if (available) hackrf_exit();
        available = false;
    }
};

/*
 *===========================================================================
 *  USRP device / channel bookkeeping
 *===========================================================================
 */
struct usrp_dev_struct
{
    int slot = 0;       // original [usrp] slot number (0..MAX_USRP-1), for serial_N/fpga/fw lookups
    std::string serial;
    int num_ch = 0;
    int ch_offset = 0; // first flat-channel index owned by this device
    uhd::usrp::multi_usrp::sptr usrp;
    uhd::rx_streamer::sptr rx_stream;
    std::thread reader_thread;
    volatile unsigned long long buff_ind = 0;
};

struct channel_buf_struct
{
    // NUM_BUFF slots, each holding samples_per_block complex<int16_t> samples
    std::vector<std::complex<int16_t>> buffer;
};

static std::mutex buff_ind_mutex;
static std::condition_variable buff_ind_cond;
static std::atomic<bool> running{false};
static std::atomic<bool> exit_flag{false};

/*
 * daq_stop.sh (and any plain `kill`/process manager) terminates this process
 * with a raw signal, not the ZMQ 'h' command -- without a handler, the
 * default action is to die immediately, skipping noise_source.close() and
 * leaving the HackRF transmitting indefinitely (its firmware TX state is
 * independent of whether the host process that started it is still alive).
 * Catching the signal just sets exit_flag so the normal shutdown path at the
 * end of main() (which stops HackRF TX and the USRP streams) still runs.
 */
static void signal_handler(int)
{
    exit_flag.store(true, std::memory_order_relaxed);
}

static void install_signal_handlers()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // deliberately no SA_RESTART: blocking calls (zmq_recv,
                      // UHD recv) must return so their loops can notice exit_flag
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(64, &sa, NULL); // matches daq_stop.sh's `pkill -64`
}

static configuration config;
static int ch_no = 0;
static int samples_per_block = 0;
static usrp_dev_struct usrp_devs[MAX_USRP];
static int n_active_devs = 0;
static std::vector<channel_buf_struct> channel_bufs;
static std::vector<int> channel_gain;      // tenths of dB, per flat channel
static long current_center_freq = 0;
static HackRFNoiseSource noise_source;

// Per-device cumulative integer-sample skip, in the same "how many samples
// to discard from this device's stream" sense as the old startup-only
// version, except this is now maintained continuously at runtime by the 's'
// command handler (see its comment) reusing delay_sync.py's own steady-state
// measurement instead of a separate, less-reliable early one-shot
// measurement. Each reader thread applies increases as it notices them.
static std::atomic<int> align_skip_samples[MAX_USRP];
// Flat-channel -> owning device list-index, built once in main() so the 's'
// handler can map a channel's correction to the right device quickly.
static int channel_to_dev[64];

// --- ZMQ-driven control state (mirrors rtl_daq.c's global flags) ---
// Protected by buff_ind_mutex, same as rtl_daq.c's single-mutex design --
// the main loop already holds it for the whole per-frame body, and the ZMQ
// thread takes it too, so en_dummy_frame/dummy_frame_cntr/the flags below
// are never touched from two different locks.
static int center_freq_change_flag = 0;
static long new_center_freq = 0;
static int gain_change_flag = 0;
static std::vector<int> new_gains;
static int noise_source_state = 0;
static int last_noise_source_state = 0;

/*
 * Wire protocol (hdaq_im_msg_struct, matching rtl_daq.c) always carries gain
 * as tenths of dB -- hw_controller.py resolves its RTL-SDR-shaped, index-into-
 * a-discrete-table gain model down to that real value before sending, using
 * self.usrp_valid_gains (see hw_controller.py) when the backend is USRP. So
 * unlike rtl_daq.c, which hands the value straight to rtlsdr_set_tuner_gain()
 * (itself snapping to the nearest of a fixed discrete step table), this side
 * never needs a lookup table of its own -- UHD's set_rx_gain() takes a plain
 * continuous dB value.
 *
 * What it does still need: RTL-SDR's driver silently snaps out-of-range
 * requests to the nearest supported step and reports what it *actually*
 * applied; UHD's set_rx_gain() gives no such feedback and silently clips to
 * the daughterboard's real gain range (not necessarily the 0-76dB assumed by
 * ini_checker.py/hw_controller.py's usrp_valid_gain_range -- that's only
 * confirmed for the specific B210 units this was tested against). Without a
 * readback, a clipped request would get echoed into if_gains as if it had
 * been applied verbatim, silently breaking any calibration logic downstream
 * (e.g. hw_controller.py's gain-consistency check at STATE_GAIN_CTR_WAIT)
 * that trusts if_gains to reflect the real hardware state.
 */
static int apply_rx_gain(usrp_dev_struct& dev, int chan, int gain_tenths_db)
{
    double requested_db = gain_tenths_db / 10.0;
    uhd::gain_range_t range = dev.usrp->get_rx_gain_range(chan);
    double clamped_db = requested_db;
    if (clamped_db < range.start()) clamped_db = range.start();
    if (clamped_db > range.stop()) clamped_db = range.stop();
    if (clamped_db != requested_db)
        log_warn("USRP %d ch %d: requested gain %.1f dB outside device range [%.1f, %.1f] dB, clamping to %.1f dB",
                 dev.slot, chan, requested_db, range.start(), range.stop(), clamped_db);

    dev.usrp->set_rx_gain(clamped_db, chan);

    double actual_db = dev.usrp->get_rx_gain(chan);
    if (std::fabs(actual_db - clamped_db) > 0.05)
        log_warn("USRP %d ch %d: gain set to %.1f dB but device reports %.1f dB", dev.slot, chan, clamped_db, actual_db);

    return (int) std::lround(actual_db * 10.0);
}

/*
 *===========================================================================
 *  Device open / PPS sync
 *===========================================================================
 */
// config.lo_offset != 0 tunes the RF LO away from the target frequency while
// UHD's DSP mixer shifts the baseband back to it, so the LO-leakage DC spike
// (inherent to the B2x0's zero-IF frontend) lands off to the side of the
// passband instead of on top of it. lo_offset == 0 keeps the previous
// behavior (LO placed directly at the target frequency).
static uhd::tune_request_t make_tune_request(double freq)
{
    if (config.lo_offset != 0)
        return uhd::tune_request_t(freq, (double) config.lo_offset);
    return uhd::tune_request_t(freq);
}

static std::string build_addr(int slot)
{
    std::string addr;
    if (strlen(config.serial[slot]) > 0)
        addr = std::string("serial=") + config.serial[slot];
    if (strlen(config.fpga_path) > 0)
    {
        if (!addr.empty()) addr += ",";
        addr += std::string("fpga=") + config.fpga_path;
    }
    if (strlen(config.fw_path) > 0)
    {
        if (!addr.empty()) addr += ",";
        addr += std::string("fw=") + config.fw_path;
    }
    return addr;
}

// PPS-synchronizes all devices' time references (device 0 is the master)
// and issues a simultaneous STREAM_MODE_START_CONTINUOUS across all of them.
// Shared by open_and_sync_devices() (first start) and
// restart_usrp_devices() (recovery after a live retune or a calibration
// restart) so both paths establish the exact same known-good multi-device
// alignment.
static void pps_sync_and_start_streams()
{
    uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
    if (n_active_devs == 1)
    {
        stream_cmd.stream_now = true;
    }
    else
    {
        // Device 0 is the PPS master (free-running/internal time reference,
        // GPIO PPS out to the others); devices 1.. lock to that PPS.
        usrp_devs[0].usrp->set_time_source("none");
        for (int d = 1; d < n_active_devs; d++)
            usrp_devs[d].usrp->set_time_source("external");

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        for (int d = 0; d < n_active_devs; d++)
            usrp_devs[d].usrp->set_time_next_pps(uhd::time_spec_t(0.0));
        std::this_thread::sleep_for(std::chrono::milliseconds(config.pps_settle_ms));

        uhd::time_spec_t t_master = usrp_devs[0].usrp->get_time_last_pps();
        for (int d = 1; d < n_active_devs; d++)
        {
            uhd::time_spec_t t_slave = usrp_devs[d].usrp->get_time_last_pps();
            double skew_us = std::abs((t_master - t_slave).get_real_secs()) * 1e6;
            log_info("Time skew USRP[0] vs USRP[%d] = %.2f us", d, skew_us);
            if (skew_us > 1000.0)
                log_warn("Large time skew on USRP %d - check PPS wiring from USRP 0", d);
        }
        stream_cmd.stream_now = false;
        stream_cmd.time_spec = uhd::time_spec_t(3.0);
    }
    for (int d = 0; d < n_active_devs; d++)
    {
        uhd::stream_cmd_t dev_cmd = stream_cmd;
        long offset_ns = config.stream_start_offset_ns[usrp_devs[d].slot];
        if (offset_ns != 0 && !dev_cmd.stream_now)
        {
            dev_cmd.time_spec = dev_cmd.time_spec - uhd::time_spec_t(offset_ns / 1.0e9);
            log_info("USRP %d: applying stream_start_offset_ns=%ld", d, offset_ns);
        }
        usrp_devs[d].rx_stream->issue_stream_cmd(dev_cmd);
    }
}

// Opens (or reopens) a single USRP session and applies the fixed
// per-session configuration -- subdev, sample rate, antenna, DC offset
// correction, and the per-channel gain already recorded in channel_gain[]
// (so a reopen restores whatever gain was live, not necessarily config.gain).
// Does not touch streaming or the streamer object. Shared by
// open_and_sync_devices() (first start, gain not live yet -> pass
// config.gain there first) and restart_usrp_devices() (reopen with the
// gain each channel already had).
static bool open_and_configure_device(int d, long freq)
{
    usrp_dev_struct& dev = usrp_devs[d];
    std::string addr = build_addr(dev.slot);
    log_info("Opening USRP %d (serial %s), args: %s", d, dev.serial.c_str(), addr.c_str());
    try
    {
        dev.usrp = uhd::usrp::multi_usrp::make(uhd::device_addr_t(addr));
    }
    catch (const uhd::exception& e)
    {
        log_fatal("Failed to open USRP %d: %s", d, e.what());
        return false;
    }

    std::string subdev_str = (dev.num_ch == 2) ? std::string(config.subdev) : std::string("A:A");
    dev.usrp->set_rx_subdev_spec(uhd::usrp::subdev_spec_t(subdev_str));
    dev.usrp->set_rx_rate((double) config.sample_rate);
    for (int c = 0; c < dev.num_ch; c++)
    {
        dev.usrp->set_rx_freq(make_tune_request((double) freq), c);
        try { dev.usrp->set_rx_dc_offset(true, c); } catch (const uhd::exception& e) {
            log_warn("USRP %d ch %d: set_rx_dc_offset not supported: %s", d, c, e.what());
        }
        channel_gain[dev.ch_offset + c] = apply_rx_gain(dev, c, channel_gain[dev.ch_offset + c]);
        dev.usrp->set_rx_antenna(config.antenna, c);
    }
    return true;
}

static bool open_and_sync_devices()
{
    // channel_gain[] hasn't been touched yet on the very first call --
    // seed it with the configured startup gain so open_and_configure_device()
    // (which re-applies whatever is already in channel_gain[]) has something
    // sane to apply.
    for (int c = 0; c < ch_no; c++) channel_gain[c] = config.gain;

    // Devices must be opened one at a time -- USB discovery isn't thread-safe.
    for (int d = 0; d < n_active_devs; d++)
        if (!open_and_configure_device(d, config.center_freq))
            return false;

    // --- Create RX streamers before PPS sync ---
    for (int d = 0; d < n_active_devs; d++)
    {
        usrp_dev_struct& dev = usrp_devs[d];
        uhd::stream_args_t args("sc16", "sc16");
        args.channels.clear();
        for (int c = 0; c < dev.num_ch; c++) args.channels.push_back((size_t) c);
        dev.rx_stream = dev.usrp->get_rx_stream(args);
    }

    pps_sync_and_start_streams();
    return true;
}

// Defined further down (per-device reader thread); forward-declared here so
// restart_usrp_devices() can respawn reader threads after reopening.
static void reader_thread_entry(int dev_idx);

// Full stop/close/reopen/restart of every USRP device, at a (possibly
// unchanged) frequency -- used both for a live frequency change and for a
// calibration restart with no frequency change (see the ZMQ 'c' handler and
// its comment). A live retune while streaming reliably overflows every
// device's small FPGA-side buffer a few seconds later (each device's
// control link blocks on LO lock for several sequential set_rx_freq()
// calls, starving the others' USB transfers the whole time) -- confirmed
// directly, and it silently drops an unknown number of samples per device,
// permanently desyncing buff_ind across devices by far more than
// STATE_SAMPLE_CAL's 's' correction is willing to touch (see
// MAX_PLAUSIBLE_PPM in the ZMQ control thread). A milder stop/retune/PPS-
// resync/restart on the *same* already-open UHD sessions was tried first
// but the reused rx_streamer never resumed producing samples after
// STREAM_MODE_STOP_CONTINUOUS on this hardware/UHD version (recv() spun on
// timeouts indefinitely) -- confirmed live. Closing and reopening each
// session from scratch, exactly like a fresh process start, is the only
// path confirmed to work, so that's what this does, just without paying the
// cost of killing and relaunching the whole process (device enumeration is
// still the slow part -- expect a multi-second gap in the data, same as
// open_and_sync_devices() takes at startup).
//
// Reader threads dereference dev.rx_stream (and indirectly dev.usrp) on
// every recv() call with no synchronization of their own, so they must be
// fully stopped and joined before any device is touched here, and only
// restarted once every device is back up -- swapping either out from under
// a live reader thread would be a data race.
static bool restart_usrp_devices(long new_center_freq_hz)
{
    running.store(false);
    for (int d = 0; d < n_active_devs; d++)
        if (usrp_devs[d].reader_thread.joinable())
            usrp_devs[d].reader_thread.join();

    for (int d = 0; d < n_active_devs; d++)
    {
        usrp_devs[d].rx_stream.reset();
        usrp_devs[d].usrp.reset();
    }

    bool ok = true;
    for (int d = 0; d < n_active_devs && ok; d++)
        ok = open_and_configure_device(d, new_center_freq_hz);

    if (ok)
    {
        for (int d = 0; d < n_active_devs; d++)
        {
            usrp_dev_struct& dev = usrp_devs[d];
            uhd::stream_args_t args("sc16", "sc16");
            args.channels.clear();
            for (int c = 0; c < dev.num_ch; c++) args.channels.push_back((size_t) c);
            dev.rx_stream = dev.usrp->get_rx_stream(args);
        }

        {
            std::lock_guard<std::mutex> lock(buff_ind_mutex);
            for (int d = 0; d < n_active_devs; d++)
                usrp_devs[d].buff_ind = 0;
        }

        pps_sync_and_start_streams();
        running.store(true);
        for (int d = 0; d < n_active_devs; d++)
            usrp_devs[d].reader_thread = std::thread(reader_thread_entry, d);
    }
    else
    {
        // Some device didn't come back -- reader threads would just
        // dereference a null usrp/rx_stream if restarted against it, so
        // don't; shut the process down cleanly instead of leaving it
        // running with no acquisition and no way to recover.
        log_fatal("Failed to reopen USRPs after retune -- acquisition is down, exiting");
        exit_flag.store(true);
        buff_ind_cond.notify_all();
    }

    return ok;
}

/*
 *===========================================================================
 *  Per-device reader thread
 *===========================================================================
 */
static void reader_thread_entry(int dev_idx)
{
    uhd::set_thread_priority_safe();
    usrp_dev_struct& dev = usrp_devs[dev_idx];
    int nch = dev.num_ch;
    uhd::rx_metadata_t meta;
    int applied_skip = 0; // how much of align_skip_samples[dev.slot] this thread has discarded so far

    while (running.load())
    {
        // Discard forward by however much the 's' command handler has
        // incremented align_skip_samples[dev.slot] since we last checked --
        // see that handler's comment for why/how this converges. Applying
        // it here (top of the fill loop, between whole CPI blocks) means it
        // never has to interrupt a block already being accumulated.
        int target_skip = align_skip_samples[dev.slot].load();
        if (target_skip > applied_skip)
        {
            int to_skip = target_skip - applied_skip;
            std::vector<std::complex<int16_t>> scratch((size_t) nch * to_skip);
            std::vector<void*> ptrs(nch);
            int skipped = 0;
            while (skipped < to_skip && running.load())
            {
                for (int c = 0; c < nch; c++)
                    ptrs[c] = scratch.data() + (size_t) c * to_skip + skipped;
                size_t n = dev.rx_stream->recv(ptrs, to_skip - skipped, meta, 1.0);
                if (n == 0) break;
                skipped += (int) n;
            }
            applied_skip += skipped;
        }

        unsigned long long wr_slot = dev.buff_ind % NUM_BUFF;
        size_t filled = 0;
        std::vector<void*> buf_ptrs(nch);
        for (int c = 0; c < nch; c++)
            buf_ptrs[c] = channel_bufs[dev.ch_offset + c].buffer.data() + wr_slot * samples_per_block;

        while (filled < (size_t) samples_per_block && running.load())
        {
            std::vector<void*> ptrs(nch);
            for (int c = 0; c < nch; c++)
                ptrs[c] = ((std::complex<int16_t>*) buf_ptrs[c]) + filled;

            size_t n = dev.rx_stream->recv(ptrs, samples_per_block - filled, meta, RECV_TIMEOUT_S);

            if (meta.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW)
                log_warn("Overflow on USRP %d", dev_idx);
            else if (meta.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE &&
                     meta.error_code != uhd::rx_metadata_t::ERROR_CODE_TIMEOUT)
                log_warn("recv() error on USRP %d: %d", dev_idx, (int) meta.error_code);

            if (n == 0) continue;
            filled += n;
        }
        if (!running.load()) break;

        {
            std::lock_guard<std::mutex> lock(buff_ind_mutex);
            dev.buff_ind++;
        }
        buff_ind_cond.notify_all();
    }
}

/*
 *===========================================================================
 *  ZMQ control thread (mirrors rtl_daq.c's fifo_read_tf)
 *===========================================================================
 */
static void* zmq_control_thread(void*)
{
    void* context = zmq_ctx_new();
    void* responder = zmq_socket(context, ZMQ_REP);
    int rcvtimeo_ms = 500; // bounds how long shutdown waits on this thread
    zmq_setsockopt(responder, ZMQ_RCVTIMEO, &rcvtimeo_ms, sizeof(rcvtimeo_ms));
    int rc = zmq_bind(responder, "tcp://*:1130");
    if (rc != 0)
    {
        log_fatal("Failed to open ZMQ socket");
        exit_flag.store(true);
        buff_ind_cond.notify_all();
        return NULL;
    }

    struct hdaq_im_msg_struct msg;
    while (!exit_flag.load())
    {
        int n = zmq_recv(responder, &msg, 128, 0);
        if (n < 0) continue; // timeout or interrupted signal -- recheck exit_flag
        log_info("IM Request from: %d", msg.source_module_identifier);
        log_info("Command id: %c", msg.command_identifier);

        std::lock_guard<std::mutex> lock(buff_ind_mutex);

        if (msg.command_identifier == 'r')
        {
            log_info("Signal 'r': Reconfiguring the tuner (center_freq/gain applied live; sample_rate change requires a restart and is ignored)");
            // center_freq is 8 byte (offset 0), sample_rate and gain are
            // 4 byte each (offsets 8, 12) -- see pack_msg_reconfiguration().
            // memcpy instead of a reinterpret_cast: msg.parameters isn't
            // 8-byte aligned within hdaq_im_msg_struct (2 single-byte fields
            // precede it), so a uint64_t* cast would be an unaligned access.
            uint64_t freq_u64; std::memcpy(&freq_u64, msg.parameters, sizeof(freq_u64));
            uint32_t gain_u32; std::memcpy(&gain_u32, msg.parameters + 12, sizeof(gain_u32));
            new_center_freq = (long) freq_u64;
            center_freq_change_flag = 1;
            for (int i = 0; i < ch_no; i++) new_gains[i] = (int) gain_u32;
            gain_change_flag = 1;
        }
        else if (msg.command_identifier == 'c')
        {
            // center_freq is 8 byte, not 4 -- 'I' tops out at ~4.295GHz,
            // below common bands like 5.8GHz (confirmed live). See
            // pack_msg_rf_tune() and the 'r' handler's comment on why this
            // is a memcpy rather than a pointer cast.
            uint64_t freq_u64; std::memcpy(&freq_u64, msg.parameters, sizeof(freq_u64));
            new_center_freq = (long) freq_u64;
            center_freq_change_flag = 1;
            log_info("Signal 'c': New center frequency: %lu MHz", (unsigned long)(freq_u64 / 1000000));
        }
        else if (msg.command_identifier == 'g')
        {
            log_info("Signal 'g': Gain tuning request");
            uint32_t* parameters = (uint32_t*) msg.parameters;
            for (int i = 0; i < ch_no; i++)
            {
                new_gains[i] = (int) parameters[i];
                log_info("Channel: %d, Gain: %.1f dB", i, (float) parameters[i] / 10);
            }
            gain_change_flag = 1;
        }
        else if (msg.command_identifier == 'a')
        {
            log_warn("Signal 'a': AGC is not supported on USRP, ignoring");
        }
        else if (msg.command_identifier == 's')
        {
            // delay_sync.py's STATE_SAMPLE_CAL sends this with a per-channel
            // ppm "correction" -- meaningless as an actual ppm trim for USRP
            // (no free-running crystal to adjust, the clock is PPS-shared),
            // but its SIGN and WHICH CHANNEL are exactly what we need: they
            // encode delay_sync's own steady-state cross-correlation
            // measurement (corr_size samples, taken after proper
            // calibration-gain tuning -- far more reliable than a one-shot
            // measurement this process could do itself at startup). Reusing
            // that measurement, a small forward-only sample skip is applied
            // each cycle in the direction it indicates, converging over
            // several of delay_sync's own retries (same closed loop it
            // already runs, just with a working correction primitive).
            //
            // Sign convention (fs_ppm_offsets[m] = -delays[m] * gain, from
            // delay_sync.py): offsets[m] > 0 means channel m is running
            // early relative to the reference -> skip that channel's own
            // device forward. offsets[m] < 0 means channel m is running
            // late -> skip device 0 (the reference all channels are measured
            // against) forward instead, letting it "catch up" to channel m.
            //
            // Deliberately NOT "skip every other device": delay_sync.py only
            // ever measures each channel against channel 0, never against
            // each other, so device 0 is the only correct target for the
            // "late" case. Nudging every other device instead created a
            // 3-device symmetric-cancellation bug: when devices 1 and 2 both
            // needed correction in the same cycle, each one's "skip the
            // other two" request gave devices 0 and 2 (say) the same net
            // skip as each other, leaving *their* relative alignment
            // unchanged despite corrections being sent every cycle.
            //
            // Only STATE_SAMPLE_CAL's *integer*-sample corrections should
            // ever be applied here -- STATE_FRAC_SAMPLE_CAL sends this same
            // command for its sub-sample residual, and a 1-sample skip is a
            // massive overshoot against a <1-sample target every single
            // time (that residual belongs to STATE_IQ_CAL's phase
            // correction, not this mechanism, which can only move in whole
            // samples). Both stages share this wire format with no state
            // indicator, but their magnitudes don't overlap: comparing
            // delay_sync.py's own gain tables, STATE_SAMPLE_CAL's smallest
            // possible correction (delay=1 sample, gain=15) is
            // 1*15*1e-7=1.5e-6, while STATE_FRAC_SAMPLE_CAL's largest
            // plausible one (|taus|=0.5, gain=20) is 0.5*20*1e-7=1.0e-6 --
            // so thresholding in that gap reliably tells them apart.
            const float INTEGER_STAGE_MIN_PPM = 1.3e-6f;
            // A USB overflow (real: happens under heavy downstream CPU/USB
            // load, e.g. a client actively consuming full-rate data) drops
            // samples on just the affected device, permanently shifting it
            // out of alignment by an essentially random, potentially huge
            // amount -- not a small fixed hardware offset. STEP=1 can't
            // converge on that in any reasonable time (a several-hundred-
            // sample jump would take minutes to hours), and chasing it is
            // pointless anyway since post-overflow the two streams are
            // genuinely discontinuous, not just shifted. ~400 samples
            // (delay_sync.py's gain=50 bracket) is a generous ceiling above
            // every legitimate one-time offset seen on this hardware so
            // far (single digits to a few dozen samples) -- above it, log
            // and wait for the next clean recalibration pass instead.
            const float MAX_PLAUSIBLE_PPM = 0.002f;
            const int STEP = 1; // samples per correction cycle -- small and monotonic, no overshoot risk
            int ref_dev_slot = usrp_devs[channel_to_dev[config.std_ch_ind]].slot;
            int ref_channel_dev = channel_to_dev[config.std_ch_ind];
            float* offsets = (float*) msg.parameters;
            for (int m = 0; m < ch_no; m++)
            {
                float mag = std::fabs(offsets[m]);
                if (mag < INTEGER_STAGE_MIN_PPM) continue; // fractional-stage request -- not ours to correct
                if (mag > MAX_PLAUSIBLE_PPM)
                {
                    log_warn("Channel %d: implausibly large correction requested (ppm=%.6f) -- "
                             "likely a post-overflow discontinuity, not applying", m, offsets[m]);
                    continue;
                }
                int d = channel_to_dev[m];
                if (d == ref_channel_dev) continue; // same device as the reference channel -- not fixable by a device-level skip
                if (offsets[m] > 0)
                    align_skip_samples[usrp_devs[d].slot].fetch_add(STEP);
                else
                    align_skip_samples[ref_dev_slot].fetch_add(STEP);
            }
            log_info("Signal 's': applied incremental per-device alignment nudges");
        }
        else if (msg.command_identifier == 'n')
        {
            noise_source_state = (msg.parameters[0] == 0) ? 0 : 1;
            log_info(noise_source_state ? "Turn on noise source" : "Turn off noise source");
        }
        else if (msg.command_identifier == 'h')
        {
            log_info("Signal 'h': halting");
            exit_flag.store(true);
        }

        en_dummy_frame = 1;
        dummy_frame_cntr = 0;
        zmq_send(responder, "ok", 2, 0);
        buff_ind_cond.notify_all();
    }
    zmq_close(responder);
    zmq_ctx_destroy(context);
    return NULL;
}

/*
 *===========================================================================
 *  main
 *===========================================================================
 */
int main(int argc, char** argv)
{
    (void) argc; (void) argv;
    log_set_level(LOG_TRACE);
    install_signal_handlers();

    memset(&config, 0, sizeof(config));
    strcpy(config.subdev, "A:A A:B");
    strcpy(config.antenna, FALLBACK_ANTENNA);
    config.pps_settle_ms = 2500;
    config.hackrf_tx_sample_rate = 8000000;
    config.hackrf_tx_gain_db = 30;

    if (ini_parse(INI_FNAME, handler, &config) < 0)
    {
        log_fatal("Configuration could not be loaded, exiting..");
        return -1;
    }
    log_set_level(config.log_level);
    ch_no = config.num_ch;
    samples_per_block = config.daq_buffer_size;
    current_center_freq = config.center_freq;

    int ch_sum = config.num_ch_dev[0] + config.num_ch_dev[1] + config.num_ch_dev[2];
    if (ch_sum != ch_no || config.num_ch_dev[0] < 1 || config.num_ch_dev[0] > 2 ||
        config.num_ch_dev[1] < 0 || config.num_ch_dev[1] > 2 ||
        config.num_ch_dev[2] < 0 || config.num_ch_dev[2] > 2)
    {
        log_fatal("Invalid [usrp] num_ch_0/1/2 (sum=%d) vs [hw] num_ch=%d", ch_sum, ch_no);
        return -1;
    }

    log_info("Starting multichannel coherent USRP receiver, %d channels", ch_no);

    // --- Build device/channel map ---
    n_active_devs = 0;
    int offset = 0;
    for (int d = 0; d < MAX_USRP; d++)
    {
        if (config.num_ch_dev[d] <= 0) continue;
        usrp_dev_struct& dev = usrp_devs[n_active_devs];
        dev.slot = d;
        dev.serial = config.serial[d];
        dev.num_ch = config.num_ch_dev[d];
        dev.ch_offset = offset;
        offset += dev.num_ch;
        n_active_devs++;
    }
    // dev.slot (the original [usrp] serial_N/num_ch_N index) is kept separate from
    // the dev's position in usrp_devs[]/n_active_devs, so a disabled middle slot
    // (e.g. num_ch_1=0) doesn't misattribute serial_2's device to list index 1.

    for (int d = 0; d < n_active_devs; d++)
        for (int c = 0; c < usrp_devs[d].num_ch; c++)
            channel_to_dev[usrp_devs[d].ch_offset + c] = d;

    channel_bufs.resize(ch_no);
    for (int c = 0; c < ch_no; c++)
        channel_bufs[c].buffer.assign((size_t) NUM_BUFF * samples_per_block, std::complex<int16_t>(0, 0));
    channel_gain.assign(ch_no, config.gain);
    new_gains.assign(ch_no, config.gain);

    // --- Noise source (non-fatal if unavailable) ---
    if (config.en_noise_source_ctr)
    {
        noise_source.init(config.hackrf_serial, config.hackrf_tx_sample_rate,
                           config.hackrf_tx_gain_db, config.hackrf_amp_enable,
                           config.hackrf_tx_freq, config.hackrf_freq_correction_ppm);
        noise_source.set_center_freq(current_center_freq);
    }

    // --- Static IQ header fields ---
    struct iq_header_struct* iq_header = (struct iq_header_struct*) calloc(1, sizeof(struct iq_header_struct));
    iq_header->sync_word = SYNC_WORD;
    iq_header->header_version = 7;
    strncpy(iq_header->hardware_id, config.hw_name ? config.hw_name : "usrp", sizeof(iq_header->hardware_id) - 1);
    iq_header->unit_id = config.hw_unit_id;
    iq_header->active_ant_chs = ch_no;
    iq_header->ioo_type = config.ioo_type;
    iq_header->rf_center_freq = (uint64_t) config.center_freq;
    iq_header->adc_sampling_freq = (uint64_t) config.sample_rate;
    iq_header->sampling_freq = (uint64_t) config.sample_rate;
    iq_header->cpi_length = (uint32_t) config.daq_buffer_size;
    iq_header->time_stamp = 0;
    iq_header->daq_block_index = 0;
    iq_header->cpi_index = 0;
    iq_header->ext_integration_cntr = 0;
    iq_header->frame_type = FRAME_TYPE_DATA;
    iq_header->data_type = 2;
    iq_header->sample_bit_depth = 16;
    iq_header->adc_overdrive_flags = 0;
    for (int m = 0; m < ch_no; m++) iq_header->if_gains[m] = (uint32_t) config.gain;
    iq_header->delay_sync_flag = 0;
    iq_header->iq_sync_flag = 0;
    iq_header->sync_state = 0;
    iq_header->noise_source_state = 0;

    // --- Open devices & PPS sync, then start streams ---
    try
    {
        if (!open_and_sync_devices())
            return -1;
    }
    catch (const uhd::exception& e)
    {
        log_fatal("UHD error during device init: %s", e.what());
        return -1;
    }

    running.store(true);
    for (int d = 0; d < n_active_devs; d++)
        usrp_devs[d].reader_thread = std::thread(reader_thread_entry, d);

    std::thread ctrl_thread(zmq_control_thread, (void*) NULL);

    unsigned long long read_buff_ind = 0;
    uint32_t overdrive_flags = 0;
    struct timeval frame_time_stamp;

    /*
     * ---> Main data acquisition loop <---
     */
    std::unique_lock<std::mutex> lock(buff_ind_mutex);
    while (!exit_flag.load())
    {
        // Bounded wait (not an unbounded wait(lock, pred)) so a signal-triggered
        // exit_flag is noticed within ~200ms even if no reader thread happens
        // to call notify_all() again (e.g. because streaming has stalled).
        bool ready = buff_ind_cond.wait_for(lock, std::chrono::milliseconds(200), [&] {
            if (exit_flag.load()) return true;
            for (int d = 0; d < n_active_devs; d++)
                if (usrp_devs[d].buff_ind <= read_buff_ind) return false;
            return true;
        });
        if (exit_flag.load()) break;
        if (!ready) continue;

        // Leaky-queue catch-up: fwrite() below can block for a long time if
        // the downstream reader (rebuffer.out) gets starved of CPU/scheduling
        // by something else on the box (a heavy consumer like a GNU Radio
        // flowgraph, in practice) -- and while blocked, this thread must NOT
        // be holding buff_ind_mutex, since every reader thread needs it just
        // to advance buff_ind after a normal (non-blocking) recv() cycle. If
        // we stayed locked through the write, a stalled consumer would stall
        // the USRP reader threads too, and a stalled reader thread means
        // nothing is draining the device's own (small, FPGA-side) buffer --
        // which overflows almost immediately, permanently desyncing that
        // device from the others (recoverable only by a full restart).
        // NUM_BUFF is sized generously so a transient stall doesn't need
        // this path, but if we still fall behind, jump forward to the
        // newest safely-readable block instead of reading one that's
        // already been (or is about to be) overwritten.
        {
            const unsigned long long SAFETY_MARGIN = 2;
            unsigned long long min_avail = usrp_devs[0].buff_ind;
            for (int d = 1; d < n_active_devs; d++)
                min_avail = std::min(min_avail, (unsigned long long) usrp_devs[d].buff_ind);
            if (min_avail > read_buff_ind + (NUM_BUFF - SAFETY_MARGIN))
            {
                unsigned long long new_read_ind = min_avail - (NUM_BUFF - SAFETY_MARGIN);
                log_warn("Falling behind consumer -- skipped %llu blocks (%llu -> %llu)",
                         new_read_ind - read_buff_ind, read_buff_ind, new_read_ind);
                read_buff_ind = new_read_ind;
            }
        }

        gettimeofday(&frame_time_stamp, NULL);
        uint64_t time_stamp_ms = (uint64_t)(frame_time_stamp.tv_sec) * 1000 + (uint64_t)(frame_time_stamp.tv_usec) / 1000;
        iq_header->time_stamp = time_stamp_ms;
        iq_header->daq_block_index = (uint32_t) read_buff_ind;
        iq_header->rf_center_freq = (uint64_t) current_center_freq;

        unsigned long long rd_slot = read_buff_ind % NUM_BUFF;
        for (int c = 0; c < ch_no; c++)
            iq_header->if_gains[c] = (uint32_t) channel_gain[c];
        iq_header->noise_source_state = (uint32_t) noise_source_state;

        bool this_frame_is_dummy = en_dummy_frame;
        if (this_frame_is_dummy)
        {
            iq_header->frame_type = FRAME_TYPE_DUMMY;
            iq_header->data_type = 0;
            iq_header->cpi_length = 0;
        }
        else
        {
            iq_header->cpi_length = (uint32_t) config.daq_buffer_size;
            iq_header->data_type = 1;
            iq_header->frame_type = (noise_source_state == 1) ? FRAME_TYPE_CAL : FRAME_TYPE_DATA;
        }

        // Everything from here to the matching lock.lock() below only reads
        // channel_bufs[*][rd_slot] -- safe unlocked because a reader thread
        // can't reach back around to overwrite rd_slot for NUM_BUFF-SAFETY_MARGIN
        // more blocks (several seconds), comfortably longer than this can take.
        lock.unlock();

        overdrive_flags = 0;
        if (!this_frame_is_dummy)
        {
            for (int c = 0; c < ch_no; c++)
            {
                const std::complex<int16_t>* buf = channel_bufs[c].buffer.data() + rd_slot * samples_per_block;
                for (int n = 0; n < samples_per_block; n++)
                {
                    if (std::abs((int) buf[n].real()) >= OVERDRIVE_THRESHOLD ||
                        std::abs((int) buf[n].imag()) >= OVERDRIVE_THRESHOLD)
                    {
                        overdrive_flags |= (1u << c);
                        break;
                    }
                }
            }
        }
        iq_header->adc_overdrive_flags = overdrive_flags;

        fwrite(iq_header, sizeof(struct iq_header_struct), 1, stdout);
        if (!this_frame_is_dummy)
        {
            for (int c = 0; c < ch_no; c++)
            {
                const std::complex<int16_t>* buf = channel_bufs[c].buffer.data() + rd_slot * samples_per_block;
                fwrite(buf, sizeof(std::complex<int16_t>), samples_per_block, stdout);
            }
        }
        if (overdrive_flags != 0) log_warn("Overdrive detected, flags: 0x%02X", overdrive_flags);
        fflush(stdout);

        lock.lock(); // reacquire -- everything below touches state shared with the ZMQ thread

        read_buff_ind++;
        if (this_frame_is_dummy)
        {
            dummy_frame_cntr++;
            if (dummy_frame_cntr == NO_DUMMY_FRAMES) en_dummy_frame = 0;
        }

        /*
         *-------------------
         *   Tuner control
         *-------------------
         * Snapshot the flags/values while locked (cheap), then release
         * before the actual UHD/HackRF calls below -- set_rx_freq/
         * set_rx_gain/hackrf_set_freq are real USB control transfers that
         * can take a non-trivial amount of time, and for the same reason
         * as the fwrite() above, must not happen while blocking every
         * reader thread from advancing (a burst of FREQ+GAIN commands --
         * exactly what a freshly-connected gr-krakensdr client sends --
         * held this section under the lock long enough to overflow the
         * USRPs in practice).
         */
        bool do_freq_change = center_freq_change_flag;
        long local_new_center_freq = new_center_freq;
        bool do_gain_change = gain_change_flag;
        std::vector<int> local_new_gains = new_gains;
        bool do_noise_toggle = (last_noise_source_state != noise_source_state) && config.en_noise_source_ctr;
        int local_noise_source_state = noise_source_state;
        center_freq_change_flag = 0;
        gain_change_flag = 0;
        last_noise_source_state = noise_source_state;

        lock.unlock();

        if (do_freq_change)
        {
            if (local_new_center_freq == current_center_freq)
            {
                // No actual retune -- this is an explicit resync-only
                // request (calibration expired without a frequency change,
                // see hw_controller.py). There's no tuning step to make
                // hitless here; what's actually broken is buff_ind
                // consistency across devices from some earlier discontinuity,
                // and a full close/reopen is the only confirmed way to
                // re-establish that (see restart_usrp_devices()'s comment).
                bool ok = restart_usrp_devices(local_new_center_freq);
                read_buff_ind = 0; // buff_ind was reset to 0 for every device above
                if (!ok)
                {
                    // restart_usrp_devices() already set exit_flag; re-lock
                    // before breaking so the unconditional lock.unlock()
                    // just after the loop (normal exit path) doesn't throw
                    // on a mutex it doesn't own.
                    lock.lock();
                    break;
                }
            }
            else
            {
                // Genuine retune. A plain sequential set_rx_freq() per
                // channel (5 blocking calls, each waiting for its own LO
                // lock) held up each device's control link long enough to
                // starve the *other* devices' still-running USB transfers,
                // overflowing their small FPGA-side buffers a few seconds
                // later (confirmed live) -- which is what actually broke
                // calibration, not the new frequency itself. Tagging every
                // channel's set_rx_freq() with the same near-future
                // set_command_time() instead schedules all of them as
                // queued register writes the devices apply on their own at
                // that instant, so this loop no longer blocks on lock-detect
                // for each of 5 channels back to back -- the streams never
                // have to stop, and the other devices' USB transfers should
                // no longer be starved. Unverified against real hardware:
                // if this still overflows, fall back to the guaranteed-safe
                // (but ~10s-of-silence) full reopen path above by sending
                // the same frequency again as a follow-up resync request.
                uhd::time_spec_t cmd_time = usrp_devs[0].usrp->get_time_now() + uhd::time_spec_t(0.1);
                for (int d = 0; d < n_active_devs; d++)
                    usrp_devs[d].usrp->set_command_time(cmd_time);
                for (int d = 0; d < n_active_devs; d++)
                    for (int c = 0; c < usrp_devs[d].num_ch; c++)
                        usrp_devs[d].usrp->set_rx_freq(make_tune_request((double) local_new_center_freq), c);
                for (int d = 0; d < n_active_devs; d++)
                    usrp_devs[d].usrp->clear_command_time();
            }
            current_center_freq = local_new_center_freq;
            noise_source.set_center_freq(current_center_freq);
            log_info("Center frequency changed to %ld Hz", current_center_freq);
        }
        if (do_gain_change)
        {
            for (int d = 0; d < n_active_devs; d++)
            {
                usrp_dev_struct& dev = usrp_devs[d];
                for (int c = 0; c < dev.num_ch; c++)
                {
                    int flat_ch = dev.ch_offset + c;
                    channel_gain[flat_ch] = apply_rx_gain(dev, c, local_new_gains[flat_ch]);
                }
            }
        }
        if (do_noise_toggle)
        {
            noise_source.set_enabled(local_noise_source_state == 1);
            log_info(local_noise_source_state ? "Noise source turned on" : "Noise source turned off");
        }

        lock.lock(); // reacquire before looping back to wait_for()
    }
    lock.unlock();

    log_info("Exiting..");
    running.store(false);
    buff_ind_cond.notify_all();
    for (int d = 0; d < n_active_devs; d++)
    {
        if (usrp_devs[d].reader_thread.joinable()) usrp_devs[d].reader_thread.join();
        // rx_stream can be null here if restart_usrp_devices() failed
        // partway through reopening a device (see its failure path).
        if (usrp_devs[d].rx_stream)
        {
            uhd::stream_cmd_t stop_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
            try { usrp_devs[d].rx_stream->issue_stream_cmd(stop_cmd); } catch (...) {}
        }
    }
    noise_source.close();
    if (ctrl_thread.joinable()) ctrl_thread.join();
    log_info("All resources released");
    return 0;
}
