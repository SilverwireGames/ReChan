import { readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

// Offline only: never collect or format while the PSP is measuring a frame.
export function validateReports(texts, expectedBuild) {
  if (!expectedBuild || texts.length !== 7) throw new Error('Expected build stamp and all seven reports required');
  let run;
  let previousUptime = 0;
  return texts.map((text, index) => {
    const values = Object.fromEntries(text.split(/\r?\n/)
      .filter(line => line.includes('='))
      .map(line => [line.slice(0, line.indexOf('=')), line.slice(line.indexOf('=') + 1)]));
    const scenario = index + 1;
    if (values.schema_version !== '2' || values.build_stamp !== expectedBuild
        || Number(values.scenario) !== scenario || Number(values.level) !== scenario
        || values.total_frames !== '600' || values.window_frames !== '600') {
      throw new Error(`S${scenario}: incomplete, stale or wrong-scenario report`);
    }
    if (!/^\d+$/.test(values.run_start_us ?? '') || Number(values.run_start_us) <= 0) {
      throw new Error(`S${scenario}: missing run identity`);
    }
    run ??= values.run_start_us;
    if (values.run_start_us !== run) throw new Error(`S${scenario}: mixed runs`);
    const uptime = Number(values.report_uptime_us);
    if (!Number.isFinite(uptime) || uptime <= Math.max(previousUptime, Number(run))) {
      throw new Error(`S${scenario}: invalid report order`);
    }
    previousUptime = uptime;
    for (const key of ['avg_fps', 'draw_avg_us', 'logic_avg_us', 'ge_sync_avg_us',
      'vblank_avg_us', 'p95_frame_us', 'palette_entries_avg']) {
      if (!(key in values) || !Number.isFinite(Number(values[key]))) {
        throw new Error(`S${scenario}: missing/invalid ${key}`);
      }
    }
    return values;
  });
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  const [, , directory, prefix, buildStamp] = process.argv;
  try {
    if (!directory || !prefix || !buildStamp) {
      throw new Error('Usage: node scripts/check_psp_benchmark.mjs DIRECTORY PREFIX "build_stamp"');
    }
    const reports = validateReports(Array.from({ length: 7 }, (_, i) =>
      readFileSync(join(directory, `${prefix}_S0${i + 1}.TXT`), 'utf8')), buildStamp);
    console.log(JSON.stringify(reports, null, 2));
  } catch (error) {
    console.error(error.message);
    process.exitCode = 1;
  }
}
