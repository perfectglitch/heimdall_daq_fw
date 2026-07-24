# Modifications to HeIMDALL DAQ Firmware

This is a modified version of [HeIMDALL DAQ Firmware](https://github.com/krakenrf/heimdall_daq_fw),
still licensed under GPLv3 (see `LICENSE`) -- this notice satisfies GPLv3
section 5(a) ("carry prominent notices stating that you modified it, and
giving a relevant date"), it does not change the license.

**Modified by:** en7
**Date:** 2026

## What changed

Added an alternative acquisition backend using 3x USRP B210-class devices
(2+2+1 = 5 coherent RX channels) plus a HackRF One as an external noise
source for amplitude/phase calibration, in place of the stock RTL-SDR/
KrakenSDR hardware path. Both backends coexist -- `make`/`make all` still
builds the original RTL-SDR-only path unchanged; `make usrp` builds this one.

New files:
- `Firmware/_daq_core/usrp_daq.cc`, `im_msg.h`
- `Firmware/usrp/coherent_usrp_source_impl.{cc,h}`, `hackrf_noise_tx_impl.{cc,h}`
  (GNU Radio OOT blocks for the companion `gr-krakensdr` project)
- `config_files/usrp_default/`
- `util/install_usrp_bundle.sh`, `util/bundled/` (see
  `util/bundled/THIRD_PARTY_NOTICE.txt` regarding the vendor FPGA/firmware
  images in that directory -- those are third-party binaries, not covered
  by this project's GPLv3 license)

Modified files (see `Firmware/BringUpInstructions.txt` step #8 for the
build/config details): `Firmware/_daq_core/Makefile`, `delay_sync.py`,
`fir_decimate.c`, `hw_controller.py`, `iq_server.c`, `rebuffer.c`,
`rtl_daq.h`, `sh_mem_util.c`, `Firmware/daq_chain_config.ini`,
`Firmware/daq_start_sm.sh`, `Firmware/daq_stop.sh`, `Firmware/ini_checker.py`,
and the `config_files/*/daq_chain_config.ini` templates.
