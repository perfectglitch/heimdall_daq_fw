/* -*- c++ -*- */
/*
 * Copyright 2026 en7
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_AOA_COHERENT_USRP_SOURCE_IMPL_H
#define INCLUDED_AOA_COHERENT_USRP_SOURCE_IMPL_H

#include <gnuradio/aoa/coherent_usrp_source.h>
#include <gnuradio/thread/thread.h>

#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/stream.hpp>
#include <uhd/types/time_spec.hpp>

#include <atomic>
#include <deque>
#include <vector>
#include <thread>

namespace gr {
  namespace aoa {

    class coherent_usrp_source_impl
        : public coherent_usrp_source
    {
    private:
      static constexpr int MAX_USRP = 4;

      // --- configuration (immutable after construction) ---
      std::string d_serial[MAX_USRP];
      int         d_num_ch[MAX_USRP];   // 0=disabled, 1 or 2=active
      double      d_center_frequency;
      double      d_sample_rate;
      double      d_gain;
      int         d_buffer_size;
      std::string d_fpga_path;
      std::string d_fw_path;

      // --- derived at construction ---
      int              d_n_usrp;              // number of active USRP slots
      int              d_n_channels;          // total output channels
      std::vector<int> d_active_slots;        // slot indices that are active
      int              d_ch_offset[MAX_USRP]; // first staging index for each slot

      // --- UHD objects (created/destroyed in start/stop) ---
      // Indexed by list index (0..d_n_usrp-1), NOT by slot number.
      std::vector<uhd::usrp::multi_usrp::sptr> d_usrp;
      std::vector<uhd::rx_streamer::sptr>       d_rx_stream;

      // --- staging / background receiver ---
      std::atomic<bool>              d_running{false};
      std::vector<std::thread>       d_recv_thread;

      gr::thread::mutex              d_staging_mutex;
      gr::thread::condition_variable d_staging_cond;
      // One deque per output channel (size d_n_channels)
      std::vector<std::deque<gr_complex>> d_staging;
      // One sample counter per active USRP (size d_n_usrp)
      std::vector<int>                    d_staging_samples;

      // --- helpers ---
      std::string build_addr(int slot) const;
      void sync_and_start_streams();
      // list_idx is the index into d_active_slots / d_usrp / d_rx_stream
      void recv_loop(int list_idx);

     public:
      coherent_usrp_source_impl(const std::string& serial_0, int num_ch_0,
                                 const std::string& serial_1, int num_ch_1,
                                 const std::string& serial_2, int num_ch_2,
                                 const std::string& serial_3, int num_ch_3,
                                 double center_frequency,
                                 double sample_rate,
                                 double gain,
                                 int    buffer_size,
                                 const std::string& fpga_path,
                                 const std::string& fw_path);
      ~coherent_usrp_source_impl();

      bool start() override;
      bool stop()  override;

      void set_center_frequency(double freq) override;
      void set_gain(double gain) override;

      int work(int noutput_items,
               gr_vector_const_void_star& input_items,
               gr_vector_void_star&       output_items) override;
    };

  } // namespace aoa
} // namespace gr

#endif /* INCLUDED_AOA_COHERENT_USRP_SOURCE_IMPL_H */
