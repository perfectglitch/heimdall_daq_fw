#!/bin/bash
#
#   Install/bundle script for the USRP B210 x3 + HackRF noise-source fork
#   of HeIMDALL DAQ Firmware, paired with the krakensdr_doa web UI.
#
#   Modeled on krakenrf's official install script:
#     https://raw.githubusercontent.com/krakenrf/krakensdr_docs/main/install_scripts/krakensdr_x86_install_doa.sh
#   but swaps the RTL-SDR/KrakenSDR acquisition path for USRP+HackRF, and
#   ships a pre-built krakensdr_doa tree (util/bundled/krakensdr_doa.tar.gz)
#   instead of git-cloning it, since the fix this fork needed
#   (kraken_sdr_receiver.py's get_iq_online() int/ndarray crash) isn't
#   upstream.
#
#   Prerequisite: this script must be run from within an ALREADY
#   USRP-patched heimdall_daq_fw checkout (i.e. Firmware/_daq_core/usrp_daq.cc,
#   util/bundled/*.bin/*.hex, Firmware/usrp/*, config_files/usrp_default/
#   already present) -- it does not fetch that source itself, only builds/installs
#   around it. It is architected to get everything else -- system deps, UHD/
#   HackRF, the krakensdr_doa Python env, directory layout -- set up so that
#   `util/../Firmware/../krakensdr_doa's kraken_doa_start.sh` works.
#
#   Target: x86_64 Linux (mirrors the reference script's scope).
#
# Deliberately no `-u`: conda's own activate/deactivate hooks (e.g. the
# mkl blas feature's deactivate.d/libblas_mkl_deactivate.sh, sourced -- not
# subshelled -- by `conda install`/`conda activate`) aren't nounset-safe
# and abort under it (CONDA_MKL_INTERFACE_LAYER_BACKUP: unbound variable).
# A well-known class of issue with conda + `set -u`, not fixable from here.
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HEIMDALL_DIR="$(dirname "$SCRIPT_DIR")"          # .../heimdall_daq_fw
PARENT_DIR="$(dirname "$HEIMDALL_DIR")"          # sibling of krakensdr_doa
KRAKENSDR_DOA_DIR="$PARENT_DIR/krakensdr_doa"    # kraken_doa_start.sh hardcodes this name

echo "heimdall_daq_fw checkout: $HEIMDALL_DIR"
echo "krakensdr_doa will be placed at: $KRAKENSDR_DOA_DIR"

#############################################
# 0. Preflight: confirm this checkout actually has the USRP patches
#############################################
missing=0
for f in \
    "Firmware/_daq_core/usrp_daq.cc" \
    "Firmware/_daq_core/im_msg.h" \
    "util/bundled/libresdr_b210.bin" \
    "util/bundled/usrp_b200_fw.hex" \
    "Firmware/usrp/coherent_usrp_source_impl.cc" \
    "config_files/usrp_default/daq_chain_config.ini"
do
    if [ ! -e "$HEIMDALL_DIR/$f" ]; then
        echo "MISSING: $f"
        missing=1
    fi
done
if [ "$missing" -eq 1 ]; then
    echo "ERROR: this heimdall_daq_fw checkout is missing USRP-fork source files."
    exit 1
fi

# Prevent sudo timeout
sudo -v
while true; do
  sudo -nv; sleep 1m
  kill -0 $$ 2>/dev/null || exit
done &
SUDO_KEEPALIVE_PID=$!
trap 'kill $SUDO_KEEPALIVE_PID 2>/dev/null || true' EXIT

#############################################
# 1. System dependencies
#############################################
sudo apt update
sudo apt -y install \
    build-essential git cmake libusb-1.0-0-dev lsof libzmq3-dev \
    clang php-cli nodejs gpsd libfftw3-bin libfftw3-dev pkg-config \
    libuhd-dev uhd-host libhackrf-dev

# Fetch stock Ettus FPGA/firmware images. Best-effort: this fork's USRP
# units are LibreSDR B220mini clones and use the vendored images under
# util/bundled/ instead (see [usrp] fpga_path/fw_path in
# daq_chain_config.ini, resolved relative to Firmware/ at runtime, hence
# "../util/bundled/...") -- uhd_images_downloader is here for completeness /
# genuine Ettus B210 users, not required for the vendored-image path.
uhd_images_downloader || echo "WARN: uhd_images_downloader failed/skipped -- fine if using the vendored LibreSDR images"

#############################################
# 2. Build the USRP DAQ backend
#############################################
cd "$HEIMDALL_DIR/Firmware/_daq_core"
make usrp

#############################################
# 3. Miniforge/conda -- isolated env for krakensdr_doa's Python stack
#############################################
if [ ! -d "$HOME/miniforge3" ]; then
    cd "$HOME"
    wget -O Miniforge3.sh https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-Linux-x86_64.sh
    chmod +x Miniforge3.sh
    ./Miniforge3.sh -b
    rm Miniforge3.sh
fi

export PATH="$HOME/miniforge3/bin:$PATH"
eval "$(conda shell.bash hook)"
conda init bash >/dev/null
conda config --set auto_activate_base false

if ! conda env list | grep -q "^kraken "; then
    conda create -y -n kraken python=3.9.7
fi
conda activate kraken

conda install -y scipy==1.9.3
conda install -y numba==0.56.4
conda install -y configparser
conda install -y pyzmq
conda install -y gitpython
conda install -y pandas
conda install -y orjson
conda install -y matplotlib
conda install -y requests
conda install -y scikit-image
conda install -y scikit-rf

# dash-bootstrap-components==1.1.0 (what krakenrf's reference script pins)
# requires dash>=2.0, which conflicts with dash==1.20.0 below (dash_devices
# 0.1.3 needs the Dash 1.x callback internals). 0.13.1 is the last release
# still compatible with Dash 1.x -- confirmed working end-to-end this
# session, unlike 1.1.0 which fails to even resolve.
pip3 install dash_bootstrap_components==0.13.1
pip3 install quart_compress==0.2.1
pip3 install quart==0.17.0
pip3 install dash_devices==0.1.3
pip3 install pyargus
pip3 install gpsd-py3

conda install -y dash==1.20.0
conda install -y werkzeug==2.0.2
conda install -y plotly==5.23.0

conda install -y "blas=*=mkl"
conda install -y numba==0.56.4
conda install -y -c numba icc_rt

#############################################
# 4. Unpack the bundled krakensdr_doa tree
#############################################
if [ -d "$KRAKENSDR_DOA_DIR" ]; then
    echo "WARN: $KRAKENSDR_DOA_DIR already exists, leaving it in place (not overwriting)"
else
    mkdir -p "$PARENT_DIR"
    tar xzf "$SCRIPT_DIR/bundled/krakensdr_doa.tar.gz" -C "$PARENT_DIR"
    mv "$PARENT_DIR/krakensdr_doa-main" "$KRAKENSDR_DOA_DIR"
fi

mkdir -p "$KRAKENSDR_DOA_DIR/_share/logs/krakensdr_doa" "$KRAKENSDR_DOA_DIR/_share/logs/heimdall_daq_fw"
if [ ! -f "$KRAKENSDR_DOA_DIR/_share/settings.json" ]; then
    cp "$SCRIPT_DIR/bundled/settings.json.template" "$KRAKENSDR_DOA_DIR/_share/settings.json"
fi
chmod +x "$KRAKENSDR_DOA_DIR/gui_run.sh" "$KRAKENSDR_DOA_DIR/kill.sh" \
         "$KRAKENSDR_DOA_DIR/util/kraken_doa_start.sh" "$KRAKENSDR_DOA_DIR/util/kraken_doa_stop.sh"
# kraken_doa_start.sh's `cd heimdall_daq_fw/Firmware` and `cd ../../krakensdr_doa`
# are relative to the WRAPPER dir (parent of both siblings), not to
# krakensdr_doa/ itself -- it belongs one level up from where it lives in
# the repo, same as the reference script's `cd ~/krakensdr_doa; cp
# krakensdr_doa/util/kraken_doa_start.sh .`
cp "$KRAKENSDR_DOA_DIR/util/kraken_doa_start.sh" "$PARENT_DIR/"
cp "$KRAKENSDR_DOA_DIR/util/kraken_doa_stop.sh" "$PARENT_DIR/"

#############################################
# 5. Optional: miniserve (Data Out Server / remote control) + node deps
#############################################
# node_modules ships inside the bundled tarball already built, so `node
# _nodejs/index.js` should work with just the apt nodejs package above.
# miniserve is optional (only needed for the remote-control HTTP endpoint,
# see krakensdr_doa's gui_run.sh) -- best-effort, skip on failure.
if ! command -v miniserve >/dev/null 2>&1; then
    sudo apt -y install rustc cargo || true
    cargo install miniserve || echo "WARN: miniserve build failed/skipped -- optional (remote control endpoint only)"
fi

#############################################
# 6. Sanity checks (best-effort -- don't fail the install if no hardware
#    is attached to the machine running this script)
#############################################
echo "--- Hardware detection (best-effort) ---"
uhd_find_devices || echo "WARN: no USRPs detected (fine if not plugged in yet)"
hackrf_info || echo "WARN: no HackRF detected (fine if not plugged in yet)"

echo ""
echo "Install complete."
echo "  heimdall_daq_fw : $HEIMDALL_DIR"
echo "  krakensdr_doa   : $KRAKENSDR_DOA_DIR"
echo ""
echo "To start: conda activate kraken && cd $PARENT_DIR && ./kraken_doa_start.sh"
echo ""
echo "Known caveat from bring-up: kraken_doa_start.sh/daq_start_sm.sh launch"
echo "hw_controller.py under 'sudo' (needed for the original RTL-SDR i2c/GPIO"
echo "bias-tee path). The USRP backend's hw_controller.py doesn't touch i2c/"
echo "GPIO at all and doesn't need root -- if this machine doesn't have"
echo "passwordless sudo configured, start hw_controller.py yourself without"
echo "sudo instead: cd $HEIMDALL_DIR/Firmware && python3 _daq_core/hw_controller.py &"
