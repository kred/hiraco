import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useReducer,
  useRef,
  type Dispatch,
  type ReactNode,
} from 'react';
import {
  initialAppState,
  type AppAction,
  type AppState,
  type PreviewBuffer,
  type SourceMetadata,
} from './appState';
import {
  CancelledError,
  HiracoWasmClient,
  type BridgeMetadata,
  type BridgePreview,
} from '../lib/wasmClient';
import { checkBrowserSupport } from '../lib/wasmSupport';
import {
  PRESET_STRENGTH,
  STAGE_KEYS,
  applyOverrides,
  builtInPresetSettings,
  makeExplicitOverrides,
  normalizeOverrides,
  serializeStageOverrides,
  type PresetId,
  type StageKey,
  type StageOverrides,
  type StageSettings,
} from '../lib/stageSettings';

/** The longest edge of the preview the browser renders. */
const PREVIEW_MAX_DIMENSION = 2560;

/** How long the browser waits for the sliders to settle before it renders. */
const PREVIEW_DELAY_MS = 250;

function reducer(state: AppState, action: AppAction): AppState {
  switch (action.type) {
    case 'source-selected':
      return {
        ...state,
        sourceName: action.name,
        originalPreview: null,
        convertedPreview: null,
        metadata: null,
        result: null,
        resultName: null,
        resultSummary: null,
        error: null,
        activity: { state: 'idle', message: `Loaded ${action.name}`, fraction: null },
      };
    case 'companion-selected':
      return { ...state, companionName: action.name };
    case 'source-prepared':
      return {
        ...state,
        imageWidth: action.width,
        imageHeight: action.height,
        metadata: action.metadata,
        baseSettings: action.baseSettings,
        settings: action.baseSettings,
        overrides: {},
        preset: null,
      };
    case 'original-preview':
      return { ...state, originalPreview: action.preview };
    case 'converted-preview':
      return { ...state, convertedPreview: action.preview };
    case 'settings-changed':
      return {
        ...state,
        settings: action.settings,
        overrides: action.overrides,
        preset: null,
      };
    case 'preset-applied':
      return {
        ...state,
        settings: action.settings,
        overrides: action.overrides,
        preset: action.preset === 'custom' ? null : action.preset,
      };
    case 'base-settings-changed':
      return { ...state, baseSettings: action.baseSettings };
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
      return { ...state, error: action.message };
    case 'module-status':
      return { ...state, moduleStatus: action.status, moduleMessage: action.message };
    case 'result-summary':
      return { ...state, resultSummary: action.summary };
    case 'result':
      return {
        ...state,
        result: action.blob,
        resultName: action.name,
        activity: { state: 'done', message: `Wrote ${action.name}`, fraction: 1 },
      };
    case 'reset':
      return initialAppState;
    default:
      return state;
  }
}

/** The settings the bridge resolves for an image, as the bridge reports them. */
function settingsFromValues(values: number[]): StageSettings {
  const settings = {} as Record<StageKey, number>;
  STAGE_KEYS.forEach((key, index) => {
    settings[key] = values[index];
  });
  return settings as StageSettings;
}

/** The overrides in the flat shape the bridge expects. */
function overridesToArray(overrides: StageOverrides): number[] {
  return Array.from(serializeStageOverrides(overrides));
}

/** The metadata of the bridge, in the shape the inspector shows. */
function metadataFromBridge(metadata: BridgeMetadata): SourceMetadata {
  return {
    make: metadata.make,
    model: metadata.model,
    uniqueCameraModel: metadata.unique_camera_model,
    librawFlip: metadata.flip,
    asShotNeutral: metadata.has_as_shot_neutral
      ? (metadata.as_shot_neutral as [number, number, number])
      : undefined,
    iso: metadata.has_exif_iso ? metadata.exif_iso : undefined,
    shutterSpeed: metadata.has_exif_shutter_speed
      ? formatShutterSpeed(metadata.exif_shutter_speed)
      : undefined,
    aperture: metadata.has_exif_aperture ? metadata.exif_aperture : undefined,
    workingWidth: metadata.has_working_geometry
      ? metadata.working_width
      : metadata.width,
    workingHeight: metadata.has_working_geometry
      ? metadata.working_height
      : metadata.height,
  };
}

/** The shutter speed the way a camera shows it. */
function formatShutterSpeed(seconds: number): string {
  if (seconds <= 0) {
    return '—';
  }
  if (seconds >= 1) {
    return `${Number(seconds.toFixed(1))} s`;
  }
  return `1/${Math.round(1 / seconds)} s`;
}

/** The preview the bridge renders, in the shape the canvas expects. */
function previewFromBridge(preview: BridgePreview): PreviewBuffer {
  return {
    width: preview.width,
    height: preview.height,
    pixels: preview.pixels,
    recoveredMask: preview.mask,
  };
}

function messageOf(error: unknown): string {
  if (error instanceof Error) {
    return error.message;
  }
  return String(error);
}

/** The file name of the Linear DNG for a source file. */
function resultNameFor(sourceName: string): string {
  const dot = sourceName.lastIndexOf('.');
  const stem = dot > 0 ? sourceName.slice(0, dot) : sourceName;
  return `${stem}.dng`;
}

interface SourceFile {
  bytes: ArrayBuffer;
  name: string;
}

interface AppActions {
  /** Opens a RAW file and prepares it in the browser. */
  selectSource(file: File): void;
  /** Attaches or removes the ORI companion of the RAW file. */
  selectCompanion(file: File | null): void;
  /** Changes one stage value and renders the converted preview again. */
  setStageValue(key: StageKey, value: number): void;
  /** Applies one of the built in presets. */
  applyPreset(preset: PresetId): void;
  /** Drops the overrides of one section. */
  resetSection(keys: StageKey[]): void;
  resetPreset(): void;
  /** Writes the Linear DNG in the browser. */
  convert(): void;
  /** Stops the running conversion. */
  cancel(): void;
  /** Saves the produced DNG. */
  download(): void;
}

interface AppStateContextValue {
  state: AppState;
  dispatch: Dispatch<AppAction>;
  actions: AppActions;
}

const AppStateContext = createContext<AppStateContextValue | null>(null);

export function AppStateProvider({ children }: { children: ReactNode }) {
  const [state, dispatch] = useReducer(reducer, initialAppState);
  const clientRef = useRef<HiracoWasmClient | null>(null);
  const sourceRef = useRef<SourceFile | null>(null);
  const companionRef = useRef<SourceFile | null>(null);
  const sessionRef = useRef<boolean>(false);
  const stateRef = useRef(state);
  stateRef.current = state;

  const previewTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const previewRunning = useRef(false);
  const previewPending = useRef(false);
  // True while openSession prepares the image: that path shows its own
  // "Preparing the image…" activity, so preview renders must not replace it.
  const sessionLoading = useRef(false);
  // Number of preview renders in flight (original and converted may run
  // together), so the activity ends only when the last one finishes.
  const previewJobs = useRef(0);

  const client = useCallback((): HiracoWasmClient => {
    if (clientRef.current === null) {
      clientRef.current = new HiracoWasmClient();
      clientRef.current.onProgress((update) => {
        dispatch({
          type: 'activity-progress',
          message: update.message,
          fraction: update.fraction,
        });
      });
    }
    return clientRef.current;
  }, []);

  // A preview render is real work that can take a while (the full resolution
  // render of an 80 megapixel RAW especially), so it is reported through the
  // same activity channel as a conversion.  The worker streams its progress
  // through the activity-progress action; the reducer only applies those
  // updates while an operation is marked as working, which is what these two
  // helpers establish and take down again.
  const beginPreviewJob = useCallback((): boolean => {
    if (sessionLoading.current) {
      return false;
    }
    previewJobs.current += 1;
    if (previewJobs.current === 1) {
      dispatch({
        type: 'activity',
        activity: { state: 'working', message: 'Rendering preview…', fraction: null },
      });
    }
    return true;
  }, []);

  const endPreviewJob = useCallback((started: boolean): void => {
    if (!started) {
      return;
    }
    previewJobs.current = Math.max(0, previewJobs.current - 1);
    if (previewJobs.current === 0) {
      dispatch({
        type: 'activity',
        activity: { state: 'idle', message: 'Ready', fraction: null },
      });
    }
  }, []);

  const renderConvertedPreview = useCallback(async (): Promise<void> => {
    if (previewRunning.current) {
      previewPending.current = true;
      return;
    }
    previewRunning.current = true;
    const started = beginPreviewJob();
    try {
      const overrides = stateRef.current.overrides;
      // At 100% zoom request the full-resolution preview; otherwise cap at
      // PREVIEW_MAX_DIMENSION so the browser doesn't allocate enormous buffers.
      const maxDimension = stateRef.current.zoomMode === '100' ? 0 : PREVIEW_MAX_DIMENSION;
      const preview = await client().convertedPreview(
        maxDimension,
        overridesToArray(overrides),
      );
      dispatch({ type: 'converted-preview', preview: previewFromBridge(preview) });
    } catch (error) {
      if (!(error instanceof CancelledError)) {
        dispatch({ type: 'error', message: messageOf(error) });
      }
    } finally {
      previewRunning.current = false;
      endPreviewJob(started);
      if (previewPending.current) {
        previewPending.current = false;
        void renderConvertedPreview();
      }
    }
  }, [beginPreviewJob, client, endPreviewJob]);

  const renderOriginalPreview = useCallback(async (): Promise<void> => {
    const started = beginPreviewJob();
    try {
      const maxDimension = stateRef.current.zoomMode === '100' ? 0 : PREVIEW_MAX_DIMENSION;
      const preview = await client().originalPreview(maxDimension);
      dispatch({ type: 'original-preview', preview: previewFromBridge(preview) });
    } catch (error) {
      if (!(error instanceof CancelledError)) {
        dispatch({ type: 'error', message: messageOf(error) });
      }
    } finally {
      endPreviewJob(started);
    }
  }, [beginPreviewJob, client, endPreviewJob]);

  const openSession = useCallback(async (): Promise<void> => {
    const source = sourceRef.current;
    if (source === null) {
      return;
    }
    dispatch({
      type: 'activity',
      activity: { state: 'working', message: 'Preparing the image…', fraction: null },
    });
    sessionLoading.current = true;
    try {
      const companion = companionRef.current;
      const opened = await client().open(
        source.bytes,
        source.name,
        companion?.bytes,
        companion?.name,
      );
      sessionRef.current = true;
      const baseValues = await client().resolvedSettings(
        overridesToArray({}),
      );
      const baseSettings = settingsFromValues(baseValues);
      dispatch({
        type: 'source-prepared',
        width: opened.width,
        height: opened.height,
        metadata: metadataFromBridge(opened.metadata),
        baseSettings,
      });
      await renderOriginalPreview();
      await renderConvertedPreview();
      dispatch({
        type: 'activity',
        activity: { state: 'idle', message: 'Ready', fraction: null },
      });
    } catch (error) {
      sessionRef.current = false;
      if (error instanceof CancelledError) {
        dispatch({
          type: 'activity',
          activity: { state: 'idle', message: 'Cancelled', fraction: null },
        });
        return;
      }
      dispatch({ type: 'error', message: messageOf(error) });
    } finally {
      sessionLoading.current = false;
    }
  }, [client, renderConvertedPreview]);

  const schedulePreview = useCallback(() => {
    if (previewTimer.current !== null) {
      clearTimeout(previewTimer.current);
    }
    previewTimer.current = setTimeout(() => {
      previewTimer.current = null;
      if (!sessionRef.current) {
        // The worker was stopped, so the image has to be prepared again
        // before it can be previewed.
        void openSession();
        return;
      }
      void renderConvertedPreview();
    }, PREVIEW_DELAY_MS);
  }, [renderConvertedPreview, openSession]);

  useEffect(() => {
    const support = checkBrowserSupport();
    if (support.registering) {
      dispatch({
        type: 'module-status',
        status: 'loading',
        message: 'Enabling cross-origin isolation…',
      });
      return;
    }
    if (!support.supported) {
      dispatch({ type: 'module-status', status: 'unsupported', message: support.reason });
      return;
    }
    dispatch({ type: 'module-status', status: 'loading', message: null });
    let cancelled = false;
    client()
      .init()
      .then(() => {
        if (!cancelled) {
          dispatch({ type: 'module-status', status: 'ready', message: null });
        }
      })
      .catch((error: unknown) => {
        if (!cancelled) {
          dispatch({
            type: 'module-status',
            status: 'unsupported',
            message: messageOf(error),
          });
        }
      });
    return () => {
      cancelled = true;
    };
  }, [client]);

  // When the zoom mode changes, re-fetch both previews at the appropriate
  // resolution: full resolution at 100%, capped at PREVIEW_MAX_DIMENSION otherwise.
  useEffect(() => {
    if (!sessionRef.current) {
      return;
    }
    void renderOriginalPreview();
    void renderConvertedPreview();
  }, [state.zoomMode, renderOriginalPreview, renderConvertedPreview]);

  useEffect(
    () => () => {
      if (previewTimer.current !== null) {
        clearTimeout(previewTimer.current);
      }
      clientRef.current?.dispose();
    },
    [],
  );

  const selectSource = useCallback(
    (file: File) => {
      void file.arrayBuffer().then((bytes) => {
        sourceRef.current = { bytes, name: file.name };
        companionRef.current = null;
        sessionRef.current = false;
        dispatch({ type: 'source-selected', name: file.name });
        dispatch({ type: 'companion-selected', name: null });
        void openSession();
      });
    },
    [openSession],
  );

  const selectCompanion = useCallback(
    (file: File | null) => {
      if (file === null) {
        companionRef.current = null;
        dispatch({ type: 'companion-selected', name: null });
        void openSession();
        return;
      }
      void file.arrayBuffer().then((bytes) => {
        companionRef.current = { bytes, name: file.name };
        dispatch({ type: 'companion-selected', name: file.name });
        void openSession();
      });
    },
    [openSession],
  );

  const setStageValue = useCallback(
    (key: StageKey, value: number) => {
      const current = stateRef.current;
      const settings: StageSettings = { ...current.settings, [key]: value };
      const overrides = normalizeOverrides(
        { ...current.overrides, [key]: value },
        current.baseSettings,
      );
      dispatch({ type: 'settings-changed', settings, overrides });
      schedulePreview();
    },
    [schedulePreview],
  );

  const applyPreset = useCallback(
    (preset: PresetId) => {
      const current = stateRef.current;
      if (preset === 'custom') {
        return;
      }
      const strength = PRESET_STRENGTH[preset];
      if (strength === undefined) {
        return;
      }
      const settings = builtInPresetSettings(current.baseSettings, strength);
      const overrides = normalizeOverrides(
        makeExplicitOverrides(settings),
        current.baseSettings,
      );
      dispatch({ type: 'preset-applied', preset, settings, overrides });
      schedulePreview();
    },
    [schedulePreview],
  );

  const resetPreset = useCallback(() => {
    const current = stateRef.current;
    dispatch({
      type: 'preset-applied',
      preset: 'custom',
      settings: current.baseSettings,
      overrides: {},
    });
    schedulePreview();
  }, [schedulePreview]);

  const resetSection = useCallback(
    (keys: StageKey[]) => {
      const current = stateRef.current;
      const overrides = { ...current.overrides };
      for (const key of keys) {
        delete overrides[key];
      }
      const nextOverrides = normalizeOverrides(overrides, current.baseSettings);
      dispatch({
        type: 'settings-changed',
        settings: applyOverrides(current.baseSettings, nextOverrides),
        overrides: nextOverrides,
      });
      schedulePreview();
    },
    [schedulePreview],
  );

  const convert = useCallback(async () => {
    const current = stateRef.current;
    const source = sourceRef.current;
    if (source === null) {
      dispatch({ type: 'error', message: 'Load a RAW file first.' });
      return;
    }
    if (!sessionRef.current) {
      await openSession();
    }
    try {
      dispatch({
        type: 'activity',
        activity: { state: 'working', message: 'Converting…', fraction: null },
      });
      const dng = await client().convert(
        current.compression,
        overridesToArray(stateRef.current.overrides),
      );
      const name = resultNameFor(source.name);
      const summary = await client().writerSummary(current.compression);
      dispatch({ type: 'result-summary', summary });
      const blob = new Blob([new Uint8Array(dng)], { type: 'image/x-adobe-dng' });
      dispatch({ type: 'result', blob, name });
      // The native application writes the file when the conversion ends,
      // so the browser hands it over as a download right away.
      saveFile(blob, name);
    } catch (error) {
      if (error instanceof CancelledError) {
        dispatch({
          type: 'activity',
          activity: { state: 'idle', message: 'Cancelled', fraction: null },
        });
        return;
      }
      dispatch({ type: 'error', message: messageOf(error) });
    }
  }, [client, openSession]);

  const cancel = useCallback(() => {
    // The conversion runs inside the module, so the worker is stopped to end
    // it.  The next worker starts without a session, so the image is
    // prepared again before the next conversion.
    sessionRef.current = false;
    client().cancel();
  }, [client]);

  const download = useCallback(() => {
    const current = stateRef.current;
    if (current.result === null) {
      return;
    }
    saveFile(current.result, current.resultName ?? 'output.dng');
  }, []);

  /** Hands a file to the browser as a download. */
function saveFile(blob: Blob, name: string): void {
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.download = name;
  anchor.style.display = 'none';
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
  // The download reads the object URL asynchronously, so it may only be
  // released once the browser picked it up.
  window.setTimeout(() => URL.revokeObjectURL(url), 30_000);
}

const actions = useMemo<AppActions>(
    () => ({
      selectSource,
      selectCompanion,
      setStageValue,
      applyPreset,
      resetSection,
      resetPreset,
      convert: () => void convert(),
      cancel,
      download,
    }),
    [
      applyPreset,
      cancel,
      convert,
      download,
      resetSection,
      selectCompanion,
      selectSource,
      setStageValue,
    ],
  );

  const value = useMemo(
    () => ({ state, dispatch, actions }),
    [state, actions],
  );
  return <AppStateContext.Provider value={value}>{children}</AppStateContext.Provider>;
}

export function useAppState(): AppStateContextValue {
  const value = useContext(AppStateContext);
  if (value === null) {
    throw new Error('useAppState must be used inside an AppStateProvider');
  }
  return value;
}
