/* -*- c++ -*- */
/*
 * Copyright 2024 en7
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_AOA_HACKRF_NOISE_TX_IMPL_H
#define INCLUDED_AOA_HACKRF_NOISE_TX_IMPL_H

#include <gnuradio/aoa/hackrf_noise_tx.h>
#include <libhackrf/hackrf.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace gr {
  namespace aoa {

    class hackrf_noise_tx_impl : public hackrf_noise_tx
    {
     private:
      double d_freq_hz;
      double d_samp_rate;
      float  d_tx_gain_db;
      bool   d_amp_enable;
      bool   d_enabled;

      // HackRF device handle
      hackrf_device* d_dev;

      // Ring buffer of int8_t interleaved IQ (produced by work(), consumed by TX callback)
      static constexpr size_t RING_SIZE = 1 << 18; // 256 k bytes
      std::vector<int8_t>     d_ring;
      size_t                  d_ring_head; // write index
      size_t                  d_ring_tail; // read index
      mutable std::mutex      d_ring_lock;
      std::condition_variable d_ring_cond;

      // Device helpers
      void open_device();
      void close_device();
      void start_tx();
      void stop_tx();

      // Static TX callback forwarded to the instance
      static int tx_callback(hackrf_transfer* transfer);
      int handle_tx(hackrf_transfer* transfer);

     public:
      hackrf_noise_tx_impl(double freq_hz,
                           double samp_rate,
                           float  tx_gain_db,
                           bool   amp_enable,
                           bool   enabled);
      ~hackrf_noise_tx_impl();

      bool start() override;
      bool stop()  override;

      int work(int noutput_items,
               gr_vector_const_void_star &input_items,
               gr_vector_void_star       &output_items) override;

      // Control API
      void set_enabled(bool enabled)       override;
      void set_freq_hz(double freq_hz)     override;
      void set_tx_gain_db(float gain_db)   override;
    };

  } // namespace aoa
} // namespace gr

#endif /* INCLUDED_AOA_HACKRF_NOISE_TX_IMPL_H */
