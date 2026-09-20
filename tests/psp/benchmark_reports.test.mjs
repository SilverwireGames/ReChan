import assert from 'node:assert/strict';
import test from 'node:test';
import { validateReports } from '../../scripts/check_psp_benchmark.mjs';

const build = 'Sep 20 2026 12:00:00';
const suite = () => Array.from({ length: 7 }, (_, i) => `schema_version=2
build_stamp=${build}
run_start_us=100
report_uptime_us=${1000 + i}
scenario=${i + 1}
level=${i + 1}
total_frames=600
window_frames=600
avg_fps=30
draw_avg_us=20000
logic_avg_us=6000
ge_sync_avg_us=25
vblank_avg_us=7000
p95_frame_us=33368
palette_entries_avg=5000
`);
test('accepts all seven scenarios from one identified build/run', () => {
  assert.equal(validateReports(suite(), build).length, 7);
});
test('rejects old builds, mixed runs, missing scenarios and truncated counters', () => {
  assert.throws(() => validateReports(suite(), 'old'));
  assert.throws(() => validateReports(suite().slice(0, 6), build));
  for (const [from, to] of [ ['run_start_us=100', 'run_start_us=101'],
    ['scenario=7', 'scenario=1'], ['palette_entries_avg=5000', ''],
    ['report_uptime_us=1006', 'report_uptime_us=99'] ]) {
    const reports = suite();
    reports[6] = reports[6].replace(from, to);
    assert.throws(() => validateReports(reports, build));
  }
});
