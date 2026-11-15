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

export type QueueStatus =
  | 'pending'
  | 'ready'
  | 'working'
  | 'done'
  | 'error'
  | 'cancelled';

/** One source in the conversion queue.  The bytes live in a ref, not here. */
export interface QueueItem {
  id: number;
  sourceName: string;
  companionName: string | null;
  status: QueueStatus;
  message: string | null;
  width: number;
  height: number;
  metadata: SourceMetadata | null;
  baseSettings: StageSettings | null;
  /** Resolved sliders for this item after it has been prepared. */
  settings: StageSettings | null;
  overrides: StageOverrides;
  preset: PresetId | null;
}

/** Settings copied from one queue entry for pasting into another. */
export interface CopiedSettings {
  settings: StageSettings;
  overrides: StageOverrides;
  preset: PresetId | null;
}

export interface AppState {
  /** The files queued for conversion and preview. */
  queue: QueueItem[];
  selectedItemId: number | null;
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
  /** The single custom preset, persisted in localStorage. */
  customPreset: StageSettings | null;
  /** Settings copied from the selected queue entry, or null. */
  copiedSettings: CopiedSettings | null;
  compression: CompressionMode;
  compareMode: CompareMode;
  zoomMode: ZoomMode;
  showRecoveredPixels: boolean;
  activity: Activity;
  error: string | null;
  /** The state of the WebAssembly module. */
  moduleStatus: ModuleStatus;
  /** Why the module cannot run, when it cannot. */
  moduleMessage: string | null;
}

export const initialAppState: AppState = {
  queue: [],
  selectedItemId: null,
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
  customPreset: null,
  copiedSettings: null,
  compression: 'deflate',
  compareMode: 'converted',
  zoomMode: 'fit',
  showRecoveredPixels: false,
  activity: { state: 'idle', message: 'Ready', fraction: null },
  error: null,
  moduleStatus: 'unknown',
  moduleMessage: null,
};

export type AppAction =
  | { type: 'queue-added'; items: QueueItem[] }
  | { type: 'queue-updated'; id: number; patch: Partial<Omit<QueueItem, 'id'>> }
  | { type: 'queue-removed'; id: number }
  | { type: 'queue-cleared' }
  | { type: 'selection-changed'; id: number | null }
  | { type: 'custom-preset-changed'; settings: StageSettings | null }
  | { type: 'settings-copied'; value: CopiedSettings | null }
  | { type: 'settings-pasted'; ids: number[]; value: CopiedSettings }
  | { type: 'source-prepared'; width: number; height: number; metadata: SourceMetadata; baseSettings: StageSettings; settings: StageSettings; overrides: StageOverrides; preset: PresetId | null }
  | { type: 'original-preview'; preview: PreviewBuffer }
  | { type: 'converted-preview'; preview: PreviewBuffer }
  | { type: 'settings-changed'; settings: StageSettings; overrides: StageOverrides }
  | { type: 'preset-applied'; preset: PresetId; settings: StageSettings; overrides: StageOverrides }
  | { type: 'compression-changed'; compression: CompressionMode }
  | { type: 'compare-mode-changed'; compareMode: CompareMode }
  | { type: 'zoom-mode-changed'; zoomMode: ZoomMode }
  | { type: 'recovered-pixels-changed'; showRecoveredPixels: boolean }
  | { type: 'activity'; activity: Activity }
  | { type: 'activity-progress'; message: string; fraction: number | null }
  | { type: 'error'; message: string | null }
  | { type: 'module-status'; status: ModuleStatus; message: string | null };
