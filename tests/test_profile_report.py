# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/profile-report.py reads a helper's profile lines and says where the threads were, what the counters cost and how long
the game went without a frame; the fixture is a few lines in the format native/proton_wine_child.c prints."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHILD = """wine-child: payload module=ntdll.prx text=800478000 bytes=606208 wired=1 errno=0 unwired_text=0
wine-child: census seconds=10 elapsed_ms=10100 files=3 dirs=1 slept_ms=10010 sampler_ms=100 other_ms=5
wine-child: census seconds=20 elapsed_ms=31000 files=9 dirs=1 slept_ms=10010 sampler_ms=100 other_ms=11000
wine-child: census_ms fds=4 image_stats=1 vm_census=10995
wine-child: thread tid=7 samples=100 in_call_stub=97 signal_blocked=1
wine-child: thread tid=8 samples=100 in_call_stub=40 signal_blocked=0
wine-child: hot rip=0x910003d40 n=40 tid=8
wine-child: hot rip=0x800478100 n=3 tid=8
wine-child: nt tid=7 call=0x1018 n=50
wine-child: file read calls=100 ms=44 slowest_ms=9 KiB=800
wine-child: section views=2 fault_calls=10 fault_ms=3 mapped=10 evicted=8 failures=0 peak=4 cached=2 map_calls=10 map_ms=1 flush_calls=8 flush_ms=900 unmap_calls=8 unmap_ms=1 reserve_calls=8 reserve_ms=1
"""
BROKER = ("gpu-pacing cookie=1 pid=1 route=direct seconds=10.0 frames=3 hidden=0 fps=0.3 mean=3000 p50=1 p95=1 p99=1 max=9000.5 ms over20=1\n"
          "gpu-pacing cookie=1 pid=1 route=direct seconds=10.0 frames=600 hidden=0 fps=60.0 mean=16 p50=16 p95=17 p99=17 max=20.0 ms over20=0\n")


class ProfileReport(unittest.TestCase):
    def test_report_names_the_costs(self):
        with tempfile.TemporaryDirectory() as folder:
            Path(folder, 'r-wine-child-5.log').write_text(CHILD)
            Path(folder, 'r-gpu-broker.log').write_text(BROKER)
            out = subprocess.run([sys.executable, str(ROOT / 'tools/profile-report.py'), 'r', '--klog', folder],
                                 check=True, capture_output=True, text=True).stdout
        self.assertIn('in call stubs 137 (68.5 %)', out)
        self.assertIn('busiest tid 8: 60 of 100 samples running', out)
        self.assertIn('guest eboot', out)
        self.assertIn('ntdll.prx+0x', out)
        self.assertIn('1 periods over 1 s of work', out)
        self.assertIn('vm walk 10995 ms', out)
        self.assertIn('more than half of the mapped pages were evicted again', out)
        self.assertIn('gaps over 1 s add up to about 9 s', out)

    def test_fixture_is_the_line_the_loader_prints(self):
        source = (ROOT / 'native/proton_payload_child.c').read_text()
        self.assertIn('"wine-child: payload module=%s text=%p bytes=%u wired=%d errno=%d unwired_text=%u\\n"', source)


if __name__ == '__main__':
    unittest.main()
