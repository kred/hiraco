import type { CompareMode, ZoomMode } from '../lib/compare';
import {
  DEFAULT_STAGE_SETTINGS,
  applyOverrides,
  presetForSettings,
  settingsEqual,
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
  /** Cached fit-mode previews, so reselecting a row does not decode again. */
  cachedOriginalPreview: PreviewBuffer | null;
  cachedConvertedPreview: PreviewBuffer | null;
}

/** Settings copied from one queue entry for pasting into another. */
export interface CopiedSettings {
  settings: StageSettings;
  overrides: StageOverrides;
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
  /** True only while a batch conversion runs; previews are not destructive. */
  converting: boolean;
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
  converting: false,
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
  | { type: 'converting'; running: boolean }
  | { type: 'activity-progress'; message: string; fraction: number | null }
  | { type: 'error'; message: string | null }
  | { type: 'module-status'; status: ModuleStatus; message: string | null };

/** Large full-resolution previews are not retained per queue row. */
const PREVIEW_CACHE_LIMIT_BYTES = 32 * 1024 * 1024;

function cacheablePreview(preview: PreviewBuffer): PreviewBuffer | null {
  return preview.pixels.byteLength <= PREVIEW_CACHE_LIMIT_BYTES ? preview : null;
}

/** The preset a value set matches: built-in first, then the custom slot. */
export function matchedPreset(
  base: StageSettings,
  settings: StageSettings,
  customPreset: StageSettings | null,
): PresetId | null {
  const builtIn = presetForSettings(base, settings);
  if (builtIn !== null) {
    return builtIn;
  }
  return customPreset !== null && settingsEqual(settings, customPreset)
    ? 'custom'
    : null;
}

export function appReducer(state: AppState, action: AppAction): AppState {
  switch (action.type) {
    case 'queue-added':
      return { ...state, queue: [...state.queue, ...action.items] };
    case 'queue-updated':
      return {
        ...state,
        queue: state.queue.map((item) =>
          item.id === action.id ? { ...item, ...action.patch } : item,
        ),
      };
    case 'queue-removed':
      return {
        ...state,
        queue: state.queue.filter((item) => item.id !== action.id),
      };
    case 'queue-cleared':
      return {
        ...state,
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
        error: null,
        activity: { state: 'idle', message: 'Ready', fraction: null },
      };
    case 'selection-changed': {
      if (action.id === null) {
        return {
          ...state,
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
          error: null,
          activity: { state: 'idle', message: 'Ready', fraction: null },
        };
      }
      const item = state.queue.find((entry) => entry.id === action.id);
      if (item === undefined) {
        return state;
      }
      const baseSettings = item.baseSettings ?? DEFAULT_STAGE_SETTINGS;
      const settings = item.settings ?? applyOverrides(baseSettings, item.overrides);
      return {
        ...state,
        selectedItemId: item.id,
        sourceName: item.sourceName,
        companionName: item.companionName,
        imageWidth: item.width,
        imageHeight: item.height,
        metadata: item.metadata,
        baseSettings,
        settings,
        overrides: item.overrides,
        preset: matchedPreset(baseSettings, settings, state.customPreset),
        // A cached preview is only valid for the fit resolution it was
        // rendered at; zoom changes request a fresh one.
        originalPreview:
          state.zoomMode === 'fit' ? item.cachedOriginalPreview : null,
        convertedPreview:
          state.zoomMode === 'fit' ? item.cachedConvertedPreview : null,
        error: null,
        activity: { state: 'idle', message: `Loading ${item.sourceName}`, fraction: null },
      };
    }
    case 'settings-copied':
      return { ...state, copiedSettings: action.value };
    case 'settings-pasted': {
      const ids = new Set(action.ids);
      const queue = state.queue.map((item) =>
        ids.has(item.id)
          ? {
              ...item,
              settings: action.value.settings,
              overrides: action.value.overrides,
              preset: item.baseSettings === null
                  ? null
                  : matchedPreset(item.baseSettings, action.value.settings, state.customPreset),
              cachedConvertedPreview: null,
            }
          : item,
      );
      if (state.selectedItemId === null || !ids.has(state.selectedItemId)) {
        return { ...state, queue };
      }
      return {
        ...state,
        queue,
        settings: action.value.settings,
        overrides: action.value.overrides,
        preset: matchedPreset(
          state.baseSettings,
          action.value.settings,
          state.customPreset,
        ),
      };
    }
    case 'custom-preset-changed': {
      const queue = state.queue.map((item) => ({
        ...item,
        preset:
          item.baseSettings !== null && item.settings !== null
            ? matchedPreset(item.baseSettings, item.settings, action.settings)
            : null,
      }));
      return {
        ...state,
        customPreset: action.settings,
        queue,
        preset:
          state.selectedItemId !== null
            ? matchedPreset(state.baseSettings, state.settings, action.settings)
            : null,
      };
    }
    case 'source-prepared': {
      return {
        ...state,
        imageWidth: action.width,
        imageHeight: action.height,
        metadata: action.metadata,
        baseSettings: action.baseSettings,
        settings: action.settings,
        overrides: action.overrides,
        preset: action.preset,
      };
    }
    case 'original-preview':
      return {
        ...state,
        originalPreview: action.preview,
        queue: state.queue.map((item) =>
          item.id === state.selectedItemId
            ? { ...item, cachedOriginalPreview: cacheablePreview(action.preview) }
            : item,
        ),
      };
    case 'converted-preview':
      return {
        ...state,
        convertedPreview: action.preview,
        queue: state.queue.map((item) =>
          item.id === state.selectedItemId
            ? { ...item, cachedConvertedPreview: cacheablePreview(action.preview) }
            : item,
        ),
      };
    case 'settings-changed': {
      // Moving a slider back onto a preset value highlights it again, like the
      // native SettingsMatchPreset.
      const preset = matchedPreset(
        state.baseSettings,
        action.settings,
        state.customPreset,
      );
      return {
        ...state,
        queue: state.queue.map((item) =>
          item.id === state.selectedItemId
            ? {
                ...item,
                settings: action.settings,
                overrides: action.overrides,
                preset,
                cachedConvertedPreview: null,
              }
            : item,
        ),
        settings: action.settings,
        overrides: action.overrides,
        preset,
      };
    }
    case 'preset-applied':
      return {
        ...state,
        queue: state.queue.map((item) =>
          item.id === state.selectedItemId
            ? {
                ...item,
                settings: action.settings,
                overrides: action.overrides,
                preset: action.preset,
                cachedConvertedPreview: null,
              }
            : item,
        ),
        settings: action.settings,
        overrides: action.overrides,
        preset: action.preset,
      };
    case 'compression-changed':
      return { ...state, compression: action.compression };
    case 'compare-mode-changed':
      return { ...state, compareMode: action.compareMode };
    case 'zoom-mode-changed':
      return { ...state, zoomMode: action.zoomMode };
    case 'recovered-pixels-changed':
      return { ...state, showRecoveredPixels: action.showRecoveredPixels };
    case 'activity':
      return { ...state, activity: action.activity };
    case 'activity-progress':
      // The progress updates arrive while an operation runs.  They only
      // fill in the message of the running operation, they never start one.
      if (state.activity.state !== 'working') {
        return state;
      }
      return {
        ...state,
        activity: { ...state.activity, message: action.message, fraction: action.fraction },
      };
    case 'error':
      return {
        ...state,
        error: action.message,
        activity: {
          state: action.message === null ? 'idle' : 'error',
          message: action.message ?? 'Ready',
          fraction: null,
        },
      };
    case 'converting':
      return { ...state, converting: action.running };
    case 'module-status':
      return { ...state, moduleStatus: action.status, moduleMessage: action.message };
    default:
      return state;
  }
}
