export type Topology = 'Linkwitz-Riley' | 'Butterworth' | 'Bessel';
export type Slope = 12 | 18 | 24 | 36 | 48;
export type SampleRate = 44100 | 48000 | 88200 | 96000 | 176400 | 192000;
export type BufferSize = 64 | 128 | 256 | 512 | 1024 | 2048 | 4096;
export type MonitorChannel = 'pgm-l' | 'pgm-r' | 'tweeter' | 'woofer' | 'mic-1' | 'mic-2' | 'mic-3' | 'mic-4' | 'mic-5' | 'mic-6';
export type PresetVersion = 'staging' | 'production';

export interface CrossoverConfig {
  enabled: boolean;
  frequency: number;
  topology: Topology;
  slope: Slope;
}

export interface BandConfig {
  id: string;
  enabled: boolean;
  frequency: number;
  q: number;
  gain: number;
}

export interface DriverConfig {
  name: string;
  delayMs: number;
  trimDb: number;
  phaseInverted: boolean;
}

export interface LimiterConfig {
  enabled: boolean;
  threshold: number;
  attack: number;
  release: number;
  ratio: number;
}

export interface MeasurementPoint {
  hz: number;
  db: number;
}

export interface MeasurementData {
  responses: {
    left: MeasurementPoint[];
    right: MeasurementPoint[];
  };
  measuredAt: string;
}

export interface ConfigState {
  input: string;
  output: string;
  sampleRate: SampleRate;
  BufferSize: BufferSize;
  micCount: number;
  volume: number;
  muted: boolean;
  crossovers: Record<string, CrossoverConfig>;
  bands: Record<string, BandConfig[]>;
  drivers: DriverConfig[];
  limiter: LimiterConfig;
  monitorChannel: MonitorChannel;
  measurement: MeasurementData | null;
}

export interface PresetFile {
  staging: ConfigState;
  production: ConfigState;
}

export const VALID_SAMPLE_RATES: SampleRate[] = [44100, 48000, 88200, 96000, 176400, 192000];
export const VALID_BUFFER_SIZES: BufferSize[] = [64, 128, 256, 512, 1024, 2048, 4096];
export const VALID_MIC_COUNTS = [2, 3, 4, 5, 6] as const;
export const VALID_TOPOLOGIES: Topology[] = ['Linkwitz-Riley', 'Butterworth', 'Bessel'];
export const VALID_SLOPES: Slope[] = [12, 18, 24, 36, 48];
export const VALID_MONITOR_CHANNELS: MonitorChannel[] = ['pgm-l', 'pgm-r', 'tweeter', 'woofer', 'mic-1', 'mic-2', 'mic-3', 'mic-4', 'mic-5', 'mic-6'];

export const DEFAULT_CROSSES: Record<string, CrossoverConfig> = {
  Woofer: { enabled: true, frequency: 2500, topology: 'Linkwitz-Riley', slope: 24 },
  Tweeter: { enabled: true, frequency: 2500, topology: 'Linkwitz-Riley', slope: 24 },
};

export const DEFAULT_DRIVERS: DriverConfig[] = [
  { name: 'Woofer', delayMs: 0, trimDb: 0, phaseInverted: false },
  { name: 'Tweeter', delayMs: 0, trimDb: 0, phaseInverted: false },
];

export const DEFAULT_STATE: ConfigState = {
  input: 'default',
  output: 'default',
  sampleRate: 48000,
  BufferSize: 512,
  micCount: 2,
  volume: 65,
  muted: false,
  crossovers: DEFAULT_CROSSES,
  bands: {
    Woofer: [],
    Tweeter: [],
  },
  drivers: DEFAULT_DRIVERS,
  limiter: { enabled: true, threshold: -1, attack: 5, release: 80, ratio: 4 },
  monitorChannel: 'tweeter',
  measurement: null,
};
