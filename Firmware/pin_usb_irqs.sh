#!/bin/bash
#
#   Pins the USB3 controller interrupts serving the attached USRP B210s to a
#   dedicated set of CPU cores, kept free of application threads (see
#   daq_start_sm.sh's taskset wrapping, which must exclude the same cores).
#
#   Why: on a PREEMPT_RT kernel, USB interrupt handling is itself a scheduled
#   SCHED_FIFO thread (irq/NNN-xhci_hcd), not a true hard-interrupt context
#   that unconditionally preempts everything the way it does on a normal
#   kernel. daq_start_sm.sh launches usrp_daq.out (and everything else in
#   the chain) under `chrt -f 99`, and child threads inherit that priority
#   unless they explicitly override it -- so several of UHD's own internal
#   USB-handling threads end up at a higher real-time priority than the
#   xhci_hcd IRQ thread (rtprio 50). If one of those threads lands on the
#   same core as a busy xhci IRQ thread, it can delay that IRQ thread enough
#   that the device's own FPGA-side buffer overflows before the host drains
#   it -- independent of recv buffer size, worse at higher sample rates
#   (more interrupts/sec needed), and invisible to CPU-utilization metrics
#   since it's a scheduling-order problem, not a capacity one. Confirmed
#   live: at 3.2 Msps across 3 USRPs, this was the actual root cause of a
#   continuous overflow that neither recv buffer tuning nor (merely
#   dynamic, in-process) CPU-affinity avoidance fully eliminated -- only
#   physically separating IRQ servicing from application threads onto
#   disjoint cores did.
#
#   Identifies which PCI USB3 controllers to pin by which ones currently
#   have a B210 (Ettus USRP, USB vendor:product 2500:0020) attached, not by
#   interrupt count -- USB enumeration completes long before any DAQ
#   traffic starts, so unlike a traffic-based heuristic, this works
#   correctly on a cold boot. Hardcoded IRQ numbers aren't used since they
#   can shift across reboots/reconnects; this re-resolves them every run.
#
#   USRP_IRQ_RESERVED_CORES (exported by daq_start_sm.sh, or defaulted
#   below for standalone runs) must list at least as many cores as there
#   are USRP-hosting USB3 controllers -- currently 3 on this box's
#   topology (2 controllers on a PCIe USB3 adapter card, 1 the host's own
#   native USB3 port). daq_start_sm.sh's own taskset exclusion list for the
#   application must be the complement of this same set.
set -eo pipefail

: "${USRP_IRQ_RESERVED_CORES:=2,5,6}"
IFS=',' read -ra RESERVED_CORES <<< "$USRP_IRQ_RESERVED_CORES"

mapfile -t pci_addrs < <(
    for dev in /sys/bus/usb/devices/*; do
        [ -f "$dev/idVendor" ] || continue
        [ "$(cat "$dev/idVendor" 2>/dev/null)" = "2500" ] || continue
        [ "$(cat "$dev/idProduct" 2>/dev/null)" = "0020" ] || continue
        realpath "$dev" | grep -oE '[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f]' | tail -1
    done | sort -u
)

if [ "${#pci_addrs[@]}" -eq 0 ]; then
    echo "pin_usb_irqs: no USRP B210 found on the USB bus yet -- skipping (nothing to pin)"
    exit 0
fi

echo "pin_usb_irqs: found ${#pci_addrs[@]} USRP-hosting USB3 controller(s): ${pci_addrs[*]}"

core_i=0
for addr in "${pci_addrs[@]}"; do
    core="${RESERVED_CORES[$((core_i % ${#RESERVED_CORES[@]}))]}"
    core_i=$((core_i + 1))

    msi_dir="/sys/bus/pci/devices/$addr/msi_irqs"
    if [ ! -d "$msi_dir" ]; then
        echo "pin_usb_irqs: $addr has no msi_irqs directory (legacy INTx IRQ?) -- skipping"
        continue
    fi

    for irq_path in "$msi_dir"/*; do
        irq_num=$(basename "$irq_path")
        if echo "$core" | sudo tee "/proc/irq/$irq_num/smp_affinity_list" > /dev/null 2>&1; then
            echo "pin_usb_irqs: pinned IRQ $irq_num (PCI $addr) -> core $core"
        else
            echo "pin_usb_irqs: WARNING failed to pin IRQ $irq_num (PCI $addr) -- continuing"
        fi
    done
done
