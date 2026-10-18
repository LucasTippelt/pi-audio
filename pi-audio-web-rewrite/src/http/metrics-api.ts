import fs from 'fs';
import os from 'os';

interface MetricsResult {
  cpu: number | null;
  cpuSource: string | null;
  cpuMessage: string;
  temperature: number | null;
  xruns: number | null;
}

let prevSample: { total: number; idle: number } | null = null;

function readProcStat(): { total: number; idle: number } | null {
  try {
    const stat = fs.readFileSync('/proc/stat', 'utf8');
    const match = stat.match(/^cpu\s+(\d+(?:\s+\d+)*)/m);
    if (!match) return null;
    const counters = match[1].trim().split(/\s+/).map(Number);
    if (counters.length < 4) return null;
    const total = counters.slice(0, 8).reduce((a, b) => a + b, 0);
    const idle = (counters[3] ?? 0) + (counters[4] ?? 0);
    return { total, idle };
  } catch {
    return null;
  }
}

function getCpuFromProcStat(): number | null {
  const current = readProcStat();
  if (!current || !prevSample) return null;

  const totalDelta = current.total - prevSample.total;
  const idleDelta = current.idle - prevSample.idle;
  if (totalDelta <= 0) return null;

  prevSample = { total: current.total, idle: current.idle };
  return Math.max(0, Math.min(100, Math.round(100 * (totalDelta - idleDelta) / totalDelta)));
}

function getCpuFromLoadAvg(): number | null {
  try {
    const load = os.loadavg();
    if (!load[0]) return null;
    const cores = os.cpus().length || 1;
    return Math.max(0, Math.min(100, Math.round(100 * load[0] / cores)));
  } catch {
    return null;
  }
}

function getTemperature(): number | null {
  try {
    const raw = fs.readFileSync('/sys/class/thermal/thermal_zone0/temp', 'utf8').trim();
    const temp = Number(raw);
    if (Number.isFinite(temp)) return Math.round((temp / 1000) * 10) / 10;
  } catch {
    // Not available
  }
  return null;
}

function getXruns(): number | null {
  const xrunPath = process.env.PI_AUDIO_XRUN_FILE || '/run/pi-audio/xruns';
  try {
    const raw = fs.readFileSync(xrunPath, 'utf8').trim();
    const val = Number(raw);
    if (Number.isFinite(val)) return Math.max(0, Math.floor(val));
  } catch {
    // Not available
  }
  return null;
}

export async function getMetrics(): Promise<MetricsResult> {
  let cpu: number | null = null;
  let cpuSource: string | null = null;

  cpu = getCpuFromProcStat();
  if (cpu !== null) {
    cpuSource = 'proc-stat';
  } else {
    cpu = getCpuFromLoadAvg();
    if (cpu !== null) {
      cpuSource = 'loadavg';
    }
  }

  const temperature = getTemperature();
  const xruns = getXruns();

  return {
    cpu,
    cpuSource,
    cpuMessage: cpu === null ? 'CPU counters and load average are unavailable.' : '',
    temperature,
    xruns,
  };
}
