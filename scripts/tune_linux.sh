#!/usr/bin/env bash
# Prints (and with --apply, applies the runtime parts of) the low-latency host
# setup used for experiment 4. Read it before running it: --apply changes
# system settings until the next reboot.
#
# Boot-time settings cannot be applied from here; add them to the kernel
# command line (e.g. GRUB_CMDLINE_LINUX in /etc/default/grub, then update-grub
# and reboot). Example for an 8-core box keeping cores 0-1 for housekeeping:
#
#   isolcpus=2-7 nohz_full=2-7 rcu_nocbs=2-7 intel_idle.max_cstate=1 processor.max_cstate=1
#   hugepagesz=2M hugepages=512 transparent_hugepage=madvise
set -euo pipefail
ISO=${ISO:-2-7}
APPLY=${1:-}

run() { echo "+ $*"; [ "$APPLY" = "--apply" ] && sudo sh -c "$*" || true; }

echo "# current kernel command line:"; cat /proc/cmdline; echo
echo "# isolated cores now: $(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo none)"
echo "# nohz_full cores now: $(cat /sys/devices/system/cpu/nohz_full 2>/dev/null || echo none)"
echo

echo "# 1. CPU frequency: fixed, no deep idle states (wake-up from C6 costs tens of us)"
run "for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > \$g; done"

echo "# 2. Keep device interrupts off the isolated cores"
run "systemctl stop irqbalance || true"
run "for f in /proc/irq/*/smp_affinity_list; do echo 0-1 > \$f 2>/dev/null || true; done"

echo "# 3. Huge pages for the shared region (hp_engine --huge)"
run "sysctl -w vm.nr_hugepages=512"
run "mkdir -p /dev/hugepages && (mountpoint -q /dev/hugepages || mount -t hugetlbfs nodev /dev/hugepages)"

echo "# 4. No page migration or swapping behind our back"
run "sysctl -w kernel.numa_balancing=0"
run "sysctl -w vm.swappiness=0"
run "echo madvise > /sys/kernel/mm/transparent_hugepage/enabled"

echo "# 5. Socket buffers big enough for market-data bursts"
run "sysctl -w net.core.rmem_max=33554432 net.core.wmem_max=33554432"

echo "# 6. Allow locking memory without root (for mlock/mlockall)"
echo "    add to /etc/security/limits.conf:  $USER - memlock unlimited"
echo
echo "# Then run, e.g.:  ISOLATED=3 CPUS=3,5 HUGE=1 ./scripts/run_experiments.sh"
echo "# Check the hot core stays quiet:  watch -n1 'grep -E \"LOC|RES|CAL\" /proc/interrupts'"
