"""
	Description :
	Unit test for the USRP resync watchdog in the Hardware Controller module
	(HWC._update_resync_watchdog) -- covers both failure modes it detects
	(track lock lost, and calibration that never converges) and the
	resync_cooldown_s rate limiting shared between them.

	Does not require any compiled DAQ core binaries, shared memory, ZMQ
	sockets, or root privileges: HWC is instantiated via __new__() (skipping
	__init__, which opens hardware/sockets and starts a control server
	thread) and only the small set of attributes _update_resync_watchdog()
	actually reads/writes are set up by hand. Time is controlled by patching
	hw_controller.monotonic instead of sleeping.

	Project : HeIMDALL DAQ Firmware
	License : GNU GPL V3
"""
import unittest
from unittest.mock import MagicMock, patch
from os.path import dirname, realpath, join
import sys

current_path  = dirname(realpath(__file__))
root_path     = dirname(dirname(current_path))
daq_core_path = join(root_path, "_daq_core")
sys.path.insert(0, daq_core_path)

from hw_controller import HWC


class FakeIQHeader:
    def __init__(self, sync_state=0, rf_center_freq=100000000):
        self.sync_state = sync_state
        self.rf_center_freq = rf_center_freq


def make_hwc(backend='usrp', stuck_cal_timeout_s=60.0, resync_cooldown_s=300.0):
    """ Builds a bare HWC instance, bypassing __init__'s hardware/socket/thread setup. """
    hwc = HWC.__new__(HWC)
    hwc.backend = backend
    hwc.module_identifier = 6
    hwc.logger = MagicMock()
    hwc.rtl_daq_socket = MagicMock()
    hwc.rtl_daq_socket.recv.return_value = b'ok'
    hwc.last_sync_state = 0
    # Matches FakeIQHeader's default rf_center_freq -- a real HWC starts this
    # at 0, which makes its very first processed frame look like a frequency
    # change too (any real center_freq differs from 0), giving one harmless
    # extra tick of grace before the stuck-timer starts. Tests that aren't
    # specifically exercising that startup edge case start pre-synced instead.
    hwc.last_rf_center_freq = 100000000
    hwc.stuck_cal_start_time = None
    hwc.last_resync_time = None
    hwc.stuck_cal_timeout_s = stuck_cal_timeout_s
    hwc.resync_cooldown_s = resync_cooldown_s
    hwc.iq_header = FakeIQHeader()
    return hwc


class TesterResyncWatchdog(unittest.TestCase):

    def setUp(self):
        patcher = patch('hw_controller.monotonic')
        self.mock_monotonic = patcher.start()
        self.addCleanup(patcher.stop)

    def _tick(self, hwc, t, sync_state, rf_center_freq=100000000):
        self.mock_monotonic.return_value = t
        hwc.iq_header.sync_state = sync_state
        hwc.iq_header.rf_center_freq = rf_center_freq
        hwc._update_resync_watchdog()

    def test_no_resync_on_a_normal_quick_lock(self):
        hwc = make_hwc()
        self._tick(hwc, 0, sync_state=1)   # STATE_INIT
        self._tick(hwc, 10, sync_state=2)  # STATE_SAMPLE_CAL
        self._tick(hwc, 20, sync_state=4)  # STATE_IQ_CAL
        self._tick(hwc, 25, sync_state=5)  # STATE_TRACK_LOCK
        self._tick(hwc, 26, sync_state=6)  # STATE_TRACK
        hwc.rtl_daq_socket.send.assert_not_called()

    def test_stuck_calibration_triggers_resync_after_timeout(self):
        hwc = make_hwc(stuck_cal_timeout_s=60.0)
        self._tick(hwc, 0, sync_state=2)
        self._tick(hwc, 30, sync_state=2)   # still under 60s -- not stuck yet
        hwc.rtl_daq_socket.send.assert_not_called()

        self._tick(hwc, 61, sync_state=2)   # 61s since the streak began -- stuck
        hwc.rtl_daq_socket.send.assert_called_once()
        self.assertEqual(hwc.last_resync_time, 61)
        self.assertIsNone(hwc.stuck_cal_start_time)

    def test_repeated_stuck_state_is_suppressed_by_cooldown(self):
        hwc = make_hwc(stuck_cal_timeout_s=60.0, resync_cooldown_s=300.0)
        self._tick(hwc, 0, sync_state=2)
        self._tick(hwc, 61, sync_state=2)   # first resync fires
        hwc.rtl_daq_socket.send.assert_called_once()

        # A fresh stuck streak begins right after (start_time was reset to
        # None), and clears its own 60s timeout well before the 300s
        # cooldown does -- must still be suppressed.
        self._tick(hwc, 62, sync_state=2)   # new streak starts
        self._tick(hwc, 130, sync_state=2)  # 68s into new streak: stuck again, but cooldown (69s since t=61) not over
        hwc.rtl_daq_socket.send.assert_called_once()  # still just the one call

    def test_resync_fires_again_once_cooldown_elapses(self):
        hwc = make_hwc(stuck_cal_timeout_s=60.0, resync_cooldown_s=300.0)
        self._tick(hwc, 0, sync_state=2)
        self._tick(hwc, 61, sync_state=2)   # first resync at t=61
        self._tick(hwc, 62, sync_state=2)   # new stuck streak starts

        self._tick(hwc, 362, sync_state=2)  # 300s since last resync, 300s into new streak
        self.assertEqual(hwc.rtl_daq_socket.send.call_count, 2)
        self.assertEqual(hwc.last_resync_time, 362)

    def test_lost_track_resyncs_immediately_without_waiting_for_stuck_timeout(self):
        hwc = make_hwc(stuck_cal_timeout_s=60.0)
        self._tick(hwc, 0, sync_state=6, rf_center_freq=100000000)   # locked
        hwc.rtl_daq_socket.send.assert_not_called()

        # Track drops one second later, same frequency -- well under the 60s
        # stuck timeout, but lost-track fires immediately regardless.
        self._tick(hwc, 1, sync_state=3, rf_center_freq=100000000)
        hwc.rtl_daq_socket.send.assert_called_once()

    def test_frequency_change_is_not_mistaken_for_lost_track_or_stuck(self):
        hwc = make_hwc(stuck_cal_timeout_s=60.0)
        self._tick(hwc, 0, sync_state=6, rf_center_freq=100000000)   # locked at freq A

        # An explicit retune: sync_state drops AND frequency changed in the
        # same step -- this is a legitimate recalibration (already handled
        # by usrp_daq.cc's own 'c' path), not a fault to resync for.
        self._tick(hwc, 1, sync_state=1, rf_center_freq=200000000)
        hwc.rtl_daq_socket.send.assert_not_called()

        # The stuck-timer grace period should restart from the retune, not
        # from before it -- 59s after the retune is still within the 60s
        # budget for the new frequency's calibration to converge.
        self._tick(hwc, 60, sync_state=2, rf_center_freq=200000000)
        hwc.rtl_daq_socket.send.assert_not_called()

    def test_non_usrp_backend_never_triggers(self):
        hwc = make_hwc(backend='rtlsdr', stuck_cal_timeout_s=60.0)
        self._tick(hwc, 0, sync_state=6)
        self._tick(hwc, 1, sync_state=1)     # would be "lost track" on USRP
        self._tick(hwc, 1000, sync_state=2)  # would be "stuck" on USRP
        hwc.rtl_daq_socket.send.assert_not_called()


if __name__ == '__main__':
    unittest.main()
