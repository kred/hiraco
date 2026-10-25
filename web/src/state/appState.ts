import type { CompareMode, ZoomMode } from '../lib/compare';
import {
  DEFAULT_STAGE_SETTINGS,
  type PresetId,
  type StageOverrides,
  type StageSettings,
} from '../lib/stageSettings';

export type CompressionMode = 'uncompressed' | 'deflate' | 'jpeg-xl';

export const COMPRESSION_MODES: { id: CompressionMode; label: string }[] = [
  { id: 'uncompressed', label: 'Uncompressed' },
  { id: 'deflate', label: 'Deflate' },
  { id: 'jpeg-xl', label: 'JPEG XL' },
];

/** A preview handed over by the wasm bridge, ready to be drawn. */
export interface PreviewBuffer {
  width: number;
  height: number;
  /** RGBA, four bytes per pixel. */
  pixels: Uint8ClampedArray;
  /**
   * One byte per pixel, 1 where the ORI highlight recovery changed the pixel.
   * Empty when the bridge has no mask for this preview.
   */
  recoveredMask: Uint8Array;
}

/** The metadata of the prepared source, as reported by the bridge. */
export interface SourceMetadata {
  make: string;
  model: string;
  uniqueCameraModel: string;
  librawFlip: number;
  asShotNeutral?: [number, number, number];
  iso?: number;
  shutterSpeed?: string;
  aperture?: number;
  focalLength?: number;
  workingWidth: number;
  workingHeight: number;
}

export type ActivityState = 'idle' | 'working' | 'error' | 'done';

/** The state of the WebAssembly module inside the browser. */
export type ModuleStatus = 'unknown' | 'loading' | 'ready' | 'unsupported';

export interface Activity {
  state: ActivityState;
  message: string;
  /** 0 .. 1, or null when the step does not report a fraction. */
  fraction: number | null;
}

export interface AppState {
  sourceName: string | null;
  companionName: string | null;
  imageWidth: number;
  imageHeight: number;
  metadata: SourceMetadata | null;
  originalPreview: PreviewBuffer | null;
  convertedPreview: PreviewBuffer | null;
  /** The settings the core resolves for this image without any override. */
  baseSettings: StageSettings;
  /** The settings the sliders show. */
  settings: StageSettings;
  overrides: StageOverrides;
  preset: PresetId | null;
  compression: CompressionMode;
  compareMode: CompareMode;
  zoomMode: ZoomMode;
  showRecoveredPixels: boolean;
  activity: Activity;
  error: string | null;
  /** The Linear DNG produced by the last conversion, as a blob. */
  result: Blob | null;
  resultName: string | null;
  /** The state of the WebAssembly module. */
  moduleStatus: ModuleStatus;
  /** Why the module cannot run, when it cannot. */
  moduleMessage: string | null;
}

export const initialAppState: AppState = {
  sourceName: null,
  companionName: null,
  imageWidth: 0,
  imageHeight: 0,
  metadata: null,
  originalPreview: null,
  convertedPreview: null,
  baseSettings: DEFAULT_STAGE_SETTINGS,
  settings: DEFAULT_STAGE_SETTINGS,
  overrides: {},
  preset: null,
  compression: 'deflate',
  compareMode: 'converted',
  zoomMode: 'fit',
  showRecoveredPixels: false,
  activity: { state: 'idle', message: 'Ready', fraction: null },
  error: null,
  result: null,
  resultName: null,
  moduleStatus: 'unknown',
  moduleMessage: null,
};

export type AppAction =
  | { type: 'source-selected'; name: string }
  | { type: 'companion-selected'; name: string | null }
  | { type: 'source-prepared'; width: number; height: number; metadata: SourceMetadata; baseSettings: StageSettings; preset: PresetId | null }
  | { type: 'original-preview'; preview: PreviewBuffer }
  | { type: 'converted-preview'; preview: PreviewBuffer }
  | { type: 'settings-changed'; settings: StageSettings; overrides: StageOverrides }
  | { type: 'preset-applied'; preset: PresetId; settings: StageSettings; overrides: StageOverrides }
  | { type: 'base-settings-changed'; baseSettings: StageSettings }
  | { type: 'compression-changed'; compression: CompressionMode }
  | { type: 'compare-mode-changed'; compareMode: CompareMode }
  | { type: 'zoom-mode-changed'; zoomMode: ZoomMode }
  | { type: 'recovered-pixels-changed'; showRecoveredPixels: boolean }
  | { type: 'activity'; activity: Activity }
  | { type: 'activity-progress'; message: string; fraction: number | null }
  | { type: 'error'; message: string | null }
  | { type: 'module-status'; status: ModuleStatus; message: string | null }
  | { type: 'result'; blob: Blob; name: string }
  | { type: 'reset' };
