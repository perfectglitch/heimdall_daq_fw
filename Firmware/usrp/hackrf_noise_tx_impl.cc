/* -*- c++ -*- */
/*
 * Copyright 2024 en7
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gnuradio/io_signature.h>
#include "hackrf_noise_tx_impl.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace gr {
  namespace aoa {

    hackrf_noise_tx::sptr
    hackrf_noise_tx::make(double freq_hz, double samp_rate, float tx_gain_db,
                          bool amp_enable, bool enabled)
    {
      return gnuradio::get_initial_sptr(
          new hackrf_noise_tx_impl(freq_hz, samp_rate, tx_gain_db, amp_enable, enabled));
    }

    hackrf_noise_tx_impl::hackrf_noise_tx_impl(
        double freq_hz, double samp_rate, float tx_gain_db,
        bool amp_enable, bool enabled)
      : gr::sync_block("hackrf_noise_tx",
            gr::io_signature::make(1, 1, sizeof(gr_complex)),
            gr::io_signature::make(0, 0, 0)),
        d_freq_hz(freq_hz),
        d_samp_rate(samp_rate),
        d_tx_gain_db(tx_gain_db),
        d_amp_enable(amp_enable),
        d_enabled(enabled),
        d_dev(nullptr),
        d_ring(RING_SIZE, 0),
        d_ring_head(0),
        d_ring_tail(0)
    {
      hackrf_init();
    }

    hackrf_noise_tx_impl::~hackrf_noise_tx_impl()
    {
      close_device();
      hackrf_exit();
    }

    // --------------------------------------------------------------------------
    // TX callback (static trampoline → instance method)
    // --------------------------------------------------------------------------

    int hackrf_noise_tx_impl::tx_callback(hackrf_transfer* transfer)
    {
      auto* self = reinterpret_cast<hackrf_noise_tx_impl*>(transfer->tx_ctx);
      return self->handle_tx(transfer);
    }

    int hackrf_noise_tx_impl::handle_tx(hackrf_transfer* transfer)
    {
      int8_t*  dst      = reinterpret_cast<int8_t*>(transfer->buffer);
      int      need     = transfer->valid_length; // bytes to fill
      int      filled   = 0;

      {
        std::unique_lock<std::mutex> lock(d_ring_lock);
        size_t avail = d_ring_head - d_ring_tail; // bytes in ring
        int    copy  = static_cast<int>(std::min(static_cast<size_t>(need), avail));

        for (int i = 0; i < copy; ++i) {
          dst[i] = d_ring[(d_ring_tail + i) & (RING_SIZE - 1)];
        }
        d_ring_tail += copy;
        filled = copy;
        d_ring_cond.notify_all();
      }

      // Pad with silence if the ring ran dry
      if (filled < need)
        std::memset(dst + filled, 0, need - filled);

      return 0; // 0 = continue streaming
    }

    // --------------------------------------------------------------------------
    // Device management
    // --------------------------------------------------------------------------

    void hackrf_noise_tx_impl::open_device()
    {
      if (d_dev) return;

      int rc = hackrf_open(&d_dev);
      if (rc != HACKRF_SUCCESS)
        throw std::runtime_error(
            std::string("hackrf_noise_tx: hackrf_open failed: ")
            + hackrf_error_name(static_cast<hackrf_error>(rc)));

      rc = hackrf_set_sample_rate(d_dev, d_samp_rate);
      if (rc != HACKRF_SUCCESS)
        GR_LOG_WARN(d_logger, "hackrf_set_sample_rate failed: "
                    + std::string(hackrf_error_name(static_cast<hackrf_error>(rc))));

      rc = hackrf_set_freq(d_dev, static_cast<uint64_t>(d_freq_hz));
      if (rc != HACKRF_SUCCESS)
        GR_LOG_WARN(d_logger, "hackrf_set_freq failed: "
                    + std::string(hackrf_error_name(static_cast<hackrf_error>(rc))));

      // TX VGA gain: 0–47 dB in 1 dB steps
      uint32_t vga = static_cast<uint32_t>(std::clamp(static_cast<int>(d_tx_gain_db), 0, 47));
      hackrf_set_txvga_gain(d_dev, vga);

      hackrf_set_amp_enable(d_dev, d_amp_enable ? 1 : 0);
    }

    void hackrf_noise_tx_impl::close_device()
    {
      if (!d_dev) return;
      hackrf_stop_tx(d_dev);
      hackrf_close(d_dev);
      d_dev = nullptr;
    }

    void hackrf_noise_tx_impl::start_tx()
    {
      if (!d_dev) return;
      int rc = hackrf_start_tx(d_dev, tx_callback, this);
      if (rc != HACKRF_SUCCESS)
        throw std::runtime_error(
            std::string("hackrf_noise_tx: hackrf_start_tx failed: ")
            + hackrf_error_name(static_cast<hackrf_error>(rc)));
    }

    void hackrf_noise_tx_impl::stop_tx()
    {
      if (!d_dev) return;
      hackrf_stop_tx(d_dev);
    }

    // --------------------------------------------------------------------------
    // GR lifecycle
    // --------------------------------------------------------------------------

    bool hackrf_noise_tx_impl::start()
    {
      open_device();
      if (d_enabled) start_tx();
      return true;
    }

    bool hackrf_noise_tx_impl::stop()
    {
      stop_tx();
      close_device();
      return true;
    }

    // --------------------------------------------------------------------------
    // work() – convert gr_complex → int8_t IQ, push into ring buffer
    // --------------------------------------------------------------------------

    int hackrf_noise_tx_impl::work(int noutput_items,
                                   gr_vector_const_void_star& input_items,
                                   gr_vector_void_star& /*output_items*/)
    {
      const gr_complex* in = reinterpret_cast<const gr_complex*>(input_items[0]);

      if (d_enabled && d_dev) {
        // Each gr_complex produces 2 int8_t bytes (I + Q)
        int bytes_needed = noutput_items * 2;
        int pushed       = 0;

        std::unique_lock<std::mutex> lock(d_ring_lock);

        // Wait up to 200 ms for space; avoids dropping under light load.
        d_ring_cond.wait_for(lock, std::chrono::milliseconds(200), [&]{
          return (RING_SIZE - (d_ring_head - d_ring_tail)) >= static_cast<size_t>(bytes_needed);
        });

        size_t free_bytes = RING_SIZE - (d_ring_head - d_ring_tail);
        int    can_push   = static_cast<int>(std::min(static_cast<size_t>(bytes_needed), free_bytes));

        for (int i = 0; i < can_push / 2; ++i) {
          float re = in[i].real();
          float im = in[i].imag();
          // clamp to [-1, 1] then scale to int8 range
          int8_t iq = static_cast<int8_t>(std::clamp(re * 127.0f, -128.0f, 127.0f));
          int8_t qq = static_cast<int8_t>(std::clamp(im * 127.0f, -128.0f, 127.0f));
          d_ring[(d_ring_head)     & (RING_SIZE - 1)] = iq;
          d_ring[(d_ring_head + 1) & (RING_SIZE - 1)] = qq;
          d_ring_head += 2;
          pushed += 2;
        }
      }

      return noutput_items;
    }

    // --------------------------------------------------------------------------
    // Control API
    // --------------------------------------------------------------------------

    void hackrf_noise_tx_impl::set_enabled(bool enabled)
    {
      if (d_enabled == enabled) return;
      d_enabled = enabled;
      if (enabled)
        start_tx();
      else
        stop_tx();
    }

    void hackrf_noise_tx_impl::set_freq_hz(double freq_hz)
    {
      d_freq_hz = freq_hz;
      if (d_dev)
        hackrf_set_freq(d_dev, static_cast<uint64_t>(d_freq_hz));
    }

    void hackrf_noise_tx_impl::set_tx_gain_db(float gain_db)
    {
      d_tx_gain_db = gain_db;
      if (d_dev) {
        uint32_t vga = static_cast<uint32_t>(std::clamp(static_cast<int>(d_tx_gain_db), 0, 47));
        hackrf_set_txvga_gain(d_dev, vga);
      }
    }

  } // namespace aoa
} // namespace gr
