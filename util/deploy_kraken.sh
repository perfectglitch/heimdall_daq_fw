#!/bin/bash
#
# Syncs this working tree directly to the remote KrakenSDR/USRP host (rsync,
# no git push/pull involved), rebuilds the DAQ core there, and restarts the
# DAQ service. Deploys whatever is on disk right now, committed or not.
#
# Usage:
#   util/deploy_kraken.sh [-y] [--force]
#
#   -y        Skip the confirmation prompt before stopping the live service.
#   --force   Deploy even if the remote checkout has its own uncommitted
#             changes (by default this aborts, since a plain rsync would
#             silently overwrite them -- that's exactly what almost got lost
#             this session when the remote had unpushed local fixes).
#
# Also syncs util/bundled/krakensdr_doa/ (tracked unpacked in this repo, not as a
# tarball -- see util/install_usrp_bundle.sh) out to its remote sibling
# directory, since kraken_doa_start.sh hardcodes that layout. Unlike the main
# repo path, krakensdr_doa's remote copy is NOT a git checkout, so this
# script can't detect if it has its own uncommitted remote-only edits the
# way it does for the main repo -- a direct edit made on the remote (as
# happened repeatedly this session before krakensdr_doa was pulled into this
# repo) will be silently overwritten by this sync. Pull any such changes
# back into this repo's util/bundled/krakensdr_doa/ first if you're not sure.
#
# Configuration (env vars, all optional):
#   KRAKEN_HOST         Remote host/IP                 (default: 192.168.1.198)
#   KRAKEN_USER         Remote SSH user                 (default: v6iku)
#   KRAKEN_REPO_PATH    Remote checkout of this repo    (default: /home/v6iku/git/heimdall_daq_fw)
#   KRAKENSDR_DOA_PATH  Remote krakensdr_doa dir         (default: sibling of KRAKEN_REPO_PATH)
#   KRAKEN_SERVICE      systemd unit to restart          (default: kraken-usrp.service)
#   KRAKEN_SSH_PASS     SSH + sudo password. Never put this in a file tracked
#                       by git -- export it in your shell for non-interactive
#                       use, or leave it unset and you'll be prompted once,
#                       locally.
#
# Rebuilding "usrp" needs libuhd-dev/libhackrf-dev on the remote, same as
# building it locally.
set -euo pipefail

KRAKEN_HOST="${KRAKEN_HOST:-192.168.1.198}"
KRAKEN_USER="${KRAKEN_USER:-v6iku}"
KRAKEN_REPO_PATH="${KRAKEN_REPO_PATH:-/home/v6iku/git/heimdall_daq_fw}"
KRAKENSDR_DOA_PATH="${KRAKENSDR_DOA_PATH:-$(dirname "$KRAKEN_REPO_PATH")/krakensdr_doa}"
KRAKEN_SERVICE="${KRAKEN_SERVICE:-kraken-usrp.service}"

ASSUME_YES=0
FORCE=0
for arg in "$@"; do
    case "$arg" in
        -y) ASSUME_YES=1 ;;
        --force) FORCE=1 ;;
        *) echo "Unknown argument: $arg" >&2; exit 1 ;;
    esac
done

REPO_ROOT="$(git rev-parse --show-toplevel)"
SSH_OPTS=(-o StrictHostKeyChecking=accept-new -o ConnectTimeout=10)

if [ -z "${KRAKEN_SSH_PASS:-}" ]; then
    read -r -s -p "SSH/sudo password for $KRAKEN_USER@$KRAKEN_HOST: " KRAKEN_SSH_PASS
    echo
fi
export SSHPASS="$KRAKEN_SSH_PASS"

ssh_run() { sshpass -e ssh "${SSH_OPTS[@]}" "$KRAKEN_USER@$KRAKEN_HOST" "$@"; }

echo "==> Checking remote checkout for uncommitted changes of its own"
remote_status="$(ssh_run "cd '$KRAKEN_REPO_PATH' && git status --porcelain" || true)"
if [ -n "$remote_status" ] && [ "$FORCE" -ne 1 ]; then
    echo "Remote checkout has its own uncommitted changes -- rsync would overwrite them:" >&2
    echo "$remote_status" >&2
    echo "Reconcile them first, or re-run with --force to overwrite anyway." >&2
    exit 1
fi

if [ "$ASSUME_YES" -ne 1 ]; then
    read -r -p "Sync this working tree (+ krakensdr_doa/) to $KRAKEN_USER@$KRAKEN_HOST:$KRAKEN_REPO_PATH and restart $KRAKEN_SERVICE? [y/N] " reply
    case "$reply" in
        [yY]|[yY][eE][sS]) ;;
        *) echo "Aborted."; exit 1 ;;
    esac
fi

echo "==> Syncing files to $KRAKEN_HOST:$KRAKEN_REPO_PATH"
# --filter=':- .gitignore' reuses the project's own ignore rules (logs,
# compiled binaries, __pycache__, runtime FIFOs under _data_control/, ...) so
# the remote's own build artifacts and live runtime state aren't touched.
# No --delete: this only ever adds/updates files, never removes anything
# that's only present on the remote side.
# krakensdr_doa/ is excluded here and synced separately below to its own
# remote sibling path -- it does NOT belong nested under KRAKEN_REPO_PATH
# on the remote (kraken_doa_start.sh hardcodes the sibling layout).
rsync -avz --exclude='.git' --exclude='__pycache__/' --exclude='/util/bundled/krakensdr_doa/' --filter=':- .gitignore' \
    -e "sshpass -e ssh ${SSH_OPTS[*]}" \
    "$REPO_ROOT/" "$KRAKEN_USER@$KRAKEN_HOST:$KRAKEN_REPO_PATH/"

echo "==> Syncing krakensdr_doa/ to $KRAKEN_HOST:$KRAKENSDR_DOA_PATH"
# krakensdr_doa/.gitignore (ui.log, _share/, settings.json*, __pycache__/,
# ...) is honored the same way via its own filter rule, keeping the remote's
# live runtime state and settings untouched.
rsync -avz --exclude='.git' --exclude='__pycache__/' --filter=':- .gitignore' \
    -e "sshpass -e ssh ${SSH_OPTS[*]}" \
    "$REPO_ROOT/util/bundled/krakensdr_doa/" "$KRAKEN_USER@$KRAKEN_HOST:$KRAKENSDR_DOA_PATH/"

echo "==> Deploying on $KRAKEN_HOST"
# Heredoc delimiter is quoted ('REMOTE_SCRIPT') so nothing inside is expanded
# locally -- every value the remote script needs is passed explicitly as a
# positional argument instead, avoiding any local/remote quoting ambiguity.
sshpass -e ssh "${SSH_OPTS[@]}" "$KRAKEN_USER@$KRAKEN_HOST" \
    bash -s -- "$KRAKEN_SSH_PASS" "$KRAKEN_REPO_PATH" "$KRAKEN_SERVICE" <<'REMOTE_SCRIPT'
set -euo pipefail
KPASS="$1"; REPO_PATH="$2"; SERVICE="$3"
SUDO() { echo "$KPASS" | sudo -S -p "" -k "$@"; }

echo "--> Validating daq_chain_config.ini against the updated schema"
cd "$REPO_PATH/Firmware"
python3 -c "
import ini_checker
p = ini_checker.read_config_file('daq_chain_config.ini')
errors = ini_checker.check_ini(p, en_hw_check=False)
if errors:
    for e in errors:
        print('CONFIG ERROR:', e)
    raise SystemExit(1)
print('Config OK')
"

echo "--> Stopping $SERVICE (so the rebuild below never overwrites live binaries)"
SUDO systemctl stop "$SERVICE" || true

echo "--> Building (make usrp)"
cd _daq_core
make usrp

echo "--> Starting $SERVICE"
SUDO systemctl start "$SERVICE"
sleep 2
SUDO systemctl status "$SERVICE" --no-pager | head -8
REMOTE_SCRIPT

echo "==> Done. Check Firmware/_logs/delay_sync.log on the remote to confirm it reaches STATE_TRACK."
