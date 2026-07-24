/* -*- c++ -*- */
/*
 * Copyright 2026 en7
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gnuradio/io_signature.h>
#include "coherent_usrp_source_impl.h"

#include <uhd/exception.hpp>
#include <uhd/utils/thread.hpp>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace gr {
  namespace aoa {

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    namespace {
      // Compute the total output channel count at construction time so it can
      // be passed to the gr::sync_block base-class io_signature before the
      // member variables are initialised.
      int count_channels(int n0, int n1, int n2, int n3) noexcept
      {
        return n0
             + std::max(0, n1)
             + std::max(0, n2)
             + std::max(0, n3);
      }
    } // anonymous namespace

    // -----------------------------------------------------------------------
    // Factory
    // -----------------------------------------------------------------------

    coherent_usrp_source::sptr
    coherent_usrp_source::make(const std::string& serial_0, int num_ch_0,
                               const std::string& serial_1, int num_ch_1,
                               const std::string& serial_2, int num_ch_2,
                               const std::string& serial_3, int num_ch_3,
                               double center_frequency,
                               double sample_rate,
                               double gain,
                               int    buffer_size,
                               const std::string& fpga_path,
                               const std::string& fw_path)
    {
      return gnuradio::get_initial_sptr(
          new coherent_usrp_source_impl(
              serial_0, num_ch_0,
              serial_1, num_ch_1,
              serial_2, num_ch_2,
              serial_3, num_ch_3,
              center_frequency, sample_rate, gain, buffer_size,
              fpga_path, fw_path));
    }

    // -----------------------------------------------------------------------
    // Constructor / destructor
    // -----------------------------------------------------------------------

    coherent_usrp_source_impl::coherent_usrp_source_impl(
        const std::string& serial_0, int num_ch_0,
        const std::string& serial_1, int num_ch_1,
        const std::string& serial_2, int num_ch_2,
        const std::string& serial_3, int num_ch_3,
        double center_frequency,
        double sample_rate,
        double gain,
        int    buffer_size,
        const std::string& fpga_path,
        const std::string& fw_path)
      : gr::sync_block("coherent_usrp_source",
                       gr::io_signature::make(0, 0, 0),
                       gr::io_signature::make(
                           count_channels(num_ch_0, num_ch_1, num_ch_2, num_ch_3),
                           count_channels(num_ch_0, num_ch_1, num_ch_2, num_ch_3),
                           sizeof(gr_complex))),
        d_center_frequency(center_frequency),
        d_sample_rate(sample_rate),
        d_gain(gain),
        d_buffer_size(buffer_size),
        d_fpga_path(fpga_path),
        d_fw_path(fw_path)
    {
      // --- store per-USRP config ---
      d_serial[0] = serial_0;  d_num_ch[0] = num_ch_0;
      d_serial[1] = serial_1;  d_num_ch[1] = num_ch_1;
      d_serial[2] = serial_2;  d_num_ch[2] = num_ch_2;
      d_serial[3] = serial_3;  d_num_ch[3] = num_ch_3;

      // --- validate ---
      if (d_num_ch[0] < 1 || d_num_ch[0] > 2)
        throw std::invalid_argument(
            "coherent_usrp_source: num_ch_0 must be 1 or 2 (USRP 0 is always active)");
      for (int i = 1; i < MAX_USRP; ++i) {
        if (d_num_ch[i] < 0 || d_num_ch[i] > 2)
          throw std::invalid_argument(
              "coherent_usrp_source: num_ch_" + std::to_string(i)
              + " must be 0 (disabled), 1, or 2");
      }

      // --- build active-slot list and per-slot channel offsets ---
      int offset = 0;
      for (int i = 0; i < MAX_USRP; ++i) {
        d_ch_offset[i] = -1;
        if (d_num_ch[i] > 0) {
          d_active_slots.push_back(i);
          d_ch_offset[i] = offset;
          offset += d_num_ch[i];
        }
      }
      d_n_usrp     = static_cast<int>(d_active_slots.size());
      d_n_channels = offset;  // == count_channels(...)

      // --- pre-size runtime vectors ---
      d_usrp       .resize(d_n_usrp);
      d_rx_stream  .resize(d_n_usrp);
      d_recv_thread.resize(d_n_usrp);
      d_staging    .resize(d_n_channels);
      d_staging_samples.assign(d_n_usrp, 0);
    }

    coherent_usrp_source_impl::~coherent_usrp_source_impl()
    {
      stop();
    }

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    std::string coherent_usrp_source_impl::build_addr(int slot) const
    {
      std::string addr;
      if (!d_serial[slot].empty())
        addr = "serial=" + d_serial[slot];
      if (!d_fpga_path.empty()) {
        if (!addr.empty()) addr += ",";
        addr += "fpga=" + d_fpga_path;
      }
      if (!d_fw_path.empty()) {
        if (!addr.empty()) addr += ",";
        addr += "fw=" + d_fw_path;
      }
      return addr;
    }

    void coherent_usrp_source_impl::sync_and_start_streams()
    {
      // Hardware note: when more than one USRP is active the onboard TCXO on
      // all devices must share a common 40 MHz reference clock (e.g. a shared
      // GPSDO wired into the clock input of each B210).  USRP 0 exports a PPS
      // pulse from its GPIO which is fed to the external PPS inputs of all
      // slave USRPs.  This is the same architecture as coherent_dual_usrp_source
      // extended to up to 4 devices.

      // --- Open all active devices sequentially ---
      // UHD device discovery over USB is not thread-safe; open one at a time.
      for (int li = 0; li < d_n_usrp; ++li) {
        int slot = d_active_slots[li];
        std::string addr = build_addr(slot);
        GR_LOG_INFO(d_logger,
            "coherent_usrp_source: opening USRP slot "
            + std::to_string(slot) + " (list idx " + std::to_string(li)
            + ") with args: " + (addr.empty() ? "(auto)" : addr));
        d_usrp[li] = uhd::usrp::multi_usrp::make(uhd::device_addr_t(addr));
        d_usrp[li]->set_rx_rate(d_sample_rate);
        for (int ch = 0; ch < d_num_ch[slot]; ++ch) {
          d_usrp[li]->set_rx_freq(uhd::tune_request_t(d_center_frequency), ch);
          d_usrp[li]->set_rx_gain(d_gain, ch);
          d_usrp[li]->set_rx_antenna("RX2", ch);
        }
      }

      // --- Create RX streamers before PPS sync ---
      for (int li = 0; li < d_n_usrp; ++li) {
        int slot = d_active_slots[li];
        uhd::stream_args_t args("fc32", "sc16");
        args.channels.clear();
        for (int ch = 0; ch < d_num_ch[slot]; ++ch)
          args.channels.push_back(static_cast<size_t>(ch));
        d_rx_stream[li] = d_usrp[li]->get_rx_stream(args);
      }

      // --- Stream command (single USRP: start now; multiple USRPs: timed) ---
      uhd::stream_cmd_t stream_cmd(
          uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);

      if (d_n_usrp == 1) {
        // No PPS synchronisation needed with a single device.
        stream_cmd.stream_now = true;
      } else {
        // Multi-USRP PPS synchronisation:
        //   USRP 0 (list index 0) acts as master (time_source="none").
        //   All other USRPs lock to the GPIO PPS exported by USRP 0
        //   (time_source="external").
        d_usrp[0]->set_time_source("none");
        for (int li = 1; li < d_n_usrp; ++li)
          d_usrp[li]->set_time_source("external");

        // Brief pause so we are not on a PPS edge.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // All devices latch time=0 on the next PPS edge.
        for (int li = 0; li < d_n_usrp; ++li)
          d_usrp[li]->set_time_next_pps(uhd::time_spec_t(0.0));

        // Wait for the PPS edge to occur (worst-case ~1 s) plus margin.
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));

        // Sanity-check: all slave timestamps should be within a few µs of master.
        uhd::time_spec_t t_master = d_usrp[0]->get_time_last_pps();
        for (int li = 1; li < d_n_usrp; ++li) {
          uhd::time_spec_t t_slave = d_usrp[li]->get_time_last_pps();
          double skew_us = std::abs((t_master - t_slave).get_real_secs()) * 1e6;
          GR_LOG_INFO(d_logger,
              "coherent_usrp_source: time skew USRP[0] vs USRP["
              + std::to_string(d_active_slots[li]) + "] = "
              + std::to_string(skew_us) + " us");
          if (skew_us > 1000.0)
            GR_LOG_WARN(d_logger,
                "coherent_usrp_source: large time skew on USRP slot "
                + std::to_string(d_active_slots[li])
                + " – check GPIO PPS wiring from USRP 0");
        }

        stream_cmd.stream_now = false;
        stream_cmd.time_spec  = uhd::time_spec_t(3.0);
      }

      for (int li = 0; li < d_n_usrp; ++li)
        d_rx_stream[li]->issue_stream_cmd(stream_cmd);
    }

    // -----------------------------------------------------------------------
    // Background receiver thread
    // -----------------------------------------------------------------------

    void coherent_usrp_source_impl::recv_loop(int list_idx)
    {
      uhd::set_thread_priority_safe();

      int slot = d_active_slots[list_idx];
      int nch  = d_num_ch[slot];
      int ch_off = d_ch_offset[slot];

      // Per-channel receive buffers
      std::vector<std::vector<gr_complex>> buf(nch);
      for (auto& b : buf)
        b.resize(d_buffer_size);

      std::vector<void*> buf_ptrs(nch);
      for (int c = 0; c < nch; ++c)
        buf_ptrs[c] = buf[c].data();

      uhd::rx_metadata_t meta;

      while (d_running.load(std::memory_order_relaxed)) {
        size_t n = d_rx_stream[list_idx]->recv(
            buf_ptrs, static_cast<size_t>(d_buffer_size),
            meta, 0.5 /* timeout s */);

        if (!d_running.load(std::memory_order_relaxed))
          break;

        if (meta.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW)
          GR_LOG_WARN(d_logger,
              "coherent_usrp_source: overflow on USRP slot "
              + std::to_string(slot));

        if (n == 0)
          continue;

        {
          gr::thread::scoped_lock lock(d_staging_mutex);
          for (int c = 0; c < nch; ++c)
            d_staging[ch_off + c].insert(
                d_staging[ch_off + c].end(),
                buf[c].begin(), buf[c].begin() + n);
          d_staging_samples[list_idx] += static_cast<int>(n);
          d_staging_cond.notify_one();
        }
      }
    }

    // -----------------------------------------------------------------------
    // GNURadio lifecycle
    // -----------------------------------------------------------------------

    bool coherent_usrp_source_impl::start()
    {
      {
        gr::thread::scoped_lock lock(d_staging_mutex);
        for (auto& dq : d_staging)
          dq.clear();
        std::fill(d_staging_samples.begin(), d_staging_samples.end(), 0);
        d_staging_cond.notify_all();
      }

      try {
        sync_and_start_streams();
      } catch (const uhd::exception& e) {
        GR_LOG_ERROR(d_logger,
            "coherent_usrp_source: UHD error during start: "
            + std::string(e.what()));
        return false;
      }

      d_running.store(true, std::memory_order_relaxed);
      for (int li = 0; li < d_n_usrp; ++li)
        d_recv_thread[li] = std::thread(
            &coherent_usrp_source_impl::recv_loop, this, li);
      return true;
    }

    bool coherent_usrp_source_impl::stop()
    {
      d_running.store(false, std::memory_order_relaxed);
      d_staging_cond.notify_all();

      for (int li = 0; li < d_n_usrp; ++li)
        if (d_recv_thread[li].joinable())
          d_recv_thread[li].join();

      uhd::stream_cmd_t stop_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
      for (int li = 0; li < d_n_usrp; ++li) {
        if (d_rx_stream[li]) {
          try { d_rx_stream[li]->issue_stream_cmd(stop_cmd); }
          catch (...) {}
          d_rx_stream[li].reset();
        }
        if (d_usrp[li])
          d_usrp[li].reset();
      }
      return true;
    }

    // -----------------------------------------------------------------------
    // Setters (thread-safe: harmless to apply while recv threads run)
    // -----------------------------------------------------------------------

    void coherent_usrp_source_impl::set_center_frequency(double freq)
    {
      d_center_frequency = freq;
      for (int li = 0; li < d_n_usrp; ++li) {
        if (!d_usrp[li]) continue;
        int slot = d_active_slots[li];
        for (int ch = 0; ch < d_num_ch[slot]; ++ch)
          d_usrp[li]->set_rx_freq(uhd::tune_request_t(freq), ch);
      }
    }

    void coherent_usrp_source_impl::set_gain(double gain)
    {
      d_gain = gain;
      for (int li = 0; li < d_n_usrp; ++li) {
        if (!d_usrp[li]) continue;
        int slot = d_active_slots[li];
        for (int ch = 0; ch < d_num_ch[slot]; ++ch)
          d_usrp[li]->set_rx_gain(gain, ch);
      }
    }

    // -----------------------------------------------------------------------
    // work()
    // -----------------------------------------------------------------------

    int coherent_usrp_source_impl::work(
        int noutput_items,
        gr_vector_const_void_star& /*input_items*/,
        gr_vector_void_star& output_items)
    {
      if (!d_running.load(std::memory_order_relaxed))
        return WORK_DONE;

      // Block until every active USRP has produced at least noutput_items
      // samples.  Using a condition variable avoids CPU-spinning.
      gr::thread::scoped_lock lock(d_staging_mutex);
      d_staging_cond.wait(lock, [&] {
        if (!d_running.load(std::memory_order_relaxed))
          return true;
        for (int li = 0; li < d_n_usrp; ++li)
          if (d_staging_samples[li] < noutput_items)
            return false;
        return true;
      });

      if (!d_running.load(std::memory_order_relaxed))
        return WORK_DONE;

      // Swap out exactly noutput_items per channel while holding the lock,
      // then release – recv threads are only blocked for this brief drain.
      std::vector<std::vector<gr_complex>> tmp(d_n_channels);
      for (int ch = 0; ch < d_n_channels; ++ch) {
        auto& dq = d_staging[ch];
        tmp[ch].assign(dq.begin(), dq.begin() + noutput_items);
        dq.erase(dq.begin(), dq.begin() + noutput_items);
      }
      for (int li = 0; li < d_n_usrp; ++li)
        d_staging_samples[li] -= noutput_items;
      lock.unlock();

      // Copy to output buffers outside the lock
      for (int ch = 0; ch < d_n_channels; ++ch) {
        gr_complex* out = reinterpret_cast<gr_complex*>(output_items[ch]);
        std::copy(tmp[ch].begin(), tmp[ch].end(), out);
      }

      return noutput_items;
    }

  } // namespace aoa
} // namespace gr
