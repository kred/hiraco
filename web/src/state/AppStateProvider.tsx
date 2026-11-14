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
  type QueueItem,
  type SourceMetadata,
} from './appState';
import { previewMaxDimension } from '../lib/compare';
import {
  loadCustomPresets,
  persistCustomPresets,
  removeCustomPreset as removeSavedPreset,
  sanitizeCustomPreset,
  upsertCustomPreset,
} from '../lib/customPresets';
import {
  CancelledError,
  HiracoWasmClient,
  ModuleCrashedError,
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
  presetForSettings,
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
          error: null,
          activity: { state: 'idle', message: 'Ready', fraction: null },
        };
      }
      const item = state.queue.find((entry) => entry.id === action.id);
      if (item === undefined) {
        return state;
      }
      const baseSettings = item.baseSettings ?? state.baseSettings;
      const settings = applyOverrides(baseSettings, state.overrides);
      const builtInPreset = presetForSettings(baseSettings, settings);
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
        preset:
          builtInPreset ??
          (state.preset === 'custom' && state.activeCustomPreset !== null
            ? 'custom'
            : null),
        originalPreview: null,
        convertedPreview: null,
        error: null,
        activity: { state: 'idle', message: `Loading ${item.sourceName}`, fraction: null },
      };
    }
    case 'custom-presets-changed':
      return {
        ...state,
        customPresets: action.presets,
        activeCustomPreset: action.active,
      };
    case 'source-prepared': {
      const settings = applyOverrides(action.baseSettings, state.overrides);
      const builtInPreset = presetForSettings(action.baseSettings, settings);
      return {
        ...state,
        imageWidth: action.width,
        imageHeight: action.height,
        metadata: action.metadata,
        baseSettings: action.baseSettings,
        settings,
        preset:
          builtInPreset ??
          (state.preset === 'custom' && state.activeCustomPreset !== null
            ? 'custom'
            : null),
      };
    }
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
    case 'module-status':
      return { ...state, moduleStatus: action.status, moduleMessage: action.message };
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

/** Case-insensitive file stem used to pair an ORF with its ORI companion. */
function fileStem(name: string): string {
  const dot = name.lastIndexOf('.');
  return (dot > 0 ? name.slice(0, dot) : name).toLowerCase();
}

function fileExtension(name: string): string {
  const dot = name.lastIndexOf('.');
  return dot >= 0 ? name.slice(dot).toLowerCase() : '';
}

function isCompanionName(name: string): boolean {
  return fileExtension(name) === '.ori';
}

interface SourceFile {
  bytes: ArrayBuffer;
  name: string;
}

interface QueueFiles {
  source: SourceFile;
  companion: SourceFile | null;
}

interface AppActions {
  /** Adds one or more RAW files, pairing ORI companions by file stem. */
  addFiles(files: File[]): void;
  /** Selects a queue entry and prepares it for preview. */
  selectQueueItem(id: number): void;
  /** Removes one queue entry. */
  removeQueueItem(id: number): void;
  /** Removes every queue entry. */
  clearQueue(): void;
  /** Attaches or removes the ORI companion of the selected RAW file. */
  selectCompanion(file: File | null): void;
  /** Changes one stage value and renders the converted preview again. */
  setStageValue(key: StageKey, value: number): void;
  /** Applies one of the built in presets. */
  applyPreset(preset: PresetId): void;
  /** Drops the overrides of one section. */
  resetSection(keys: StageKey[]): void;
  /** Selects a saved custom preset without applying it yet. */
  selectCustomPreset(name: string | null): void;
  /** Saves the current absolute settings under a name. */
  saveCustomPreset(name: string): void;
  /** Applies the named saved preset. */
  loadCustomPreset(name: string): void;
  /** Deletes the named saved preset. */
  deleteCustomPreset(name: string): void;
  /** Writes a Linear DNG for every queued file. */
  convert(): void;
  /** Stops the running conversion. */
  cancel(): void;
}

interface AppStateContextValue {
  state: AppState;
  dispatch: Dispatch<AppAction>;
  actions: AppActions;
}

const AppStateContext = createContext<AppStateContextValue | null>(null);

export function AppStateProvider({ children }: { children: ReactNode }) {
  const [state, dispatch] = useReducer(
    reducer,
    initialAppState,
    (initial) => ({ ...initial, customPresets: loadCustomPresets() }),
  );
  const clientRef = useRef<HiracoWasmClient | null>(null);
  const sourceRef = useRef<SourceFile | null>(null);
  const companionRef = useRef<SourceFile | null>(null);
  const sessionRef = useRef<boolean>(false);
  const stateRef = useRef(state);
  stateRef.current = state;
  // File bytes are deliberately outside React state: a queue of large ORFs
  // must not be copied by every reducer update.
  const queueFiles = useRef<Map<number, QueueFiles>>(new Map());
  const nextItemId = useRef(1);
  const activeItemId = useRef<number | null>(null);
  // Incremented by Clear all so an in-flight add cannot repopulate the queue.
  const queueEpoch = useRef(0);
  // Incremented whenever the selected queue entry changes; late preview/open
  // replies from the previous entry are discarded instead of overwriting it.
  const selectionGeneration = useRef(0);
  const convertingRef = useRef(false);

  const previewTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const previewRunning = useRef(false);
  const previewPending = useRef(false);
  const originalRunning = useRef(false);
  const originalPending = useRef(false);
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
    if (convertingRef.current) {
      return;
    }
    if (previewRunning.current) {
      previewPending.current = true;
      return;
    }
    previewRunning.current = true;
    const generation = selectionGeneration.current;
    const started = beginPreviewJob();
    try {
      const overrides = stateRef.current.overrides;
      const maxDimension = previewMaxDimension(
        stateRef.current.zoomMode,
        stateRef.current.imageWidth,
        PREVIEW_MAX_DIMENSION,
      );
      const preview = await client().convertedPreview(
        maxDimension,
        overridesToArray(overrides),
      );
      if (generation !== selectionGeneration.current) {
        return;
      }
      dispatch({ type: 'converted-preview', preview: previewFromBridge(preview) });
    } catch (error) {
      if (generation !== selectionGeneration.current) {
        return;
      }
      if (error instanceof ModuleCrashedError) {
        sessionRef.current = false;
      }
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
    if (convertingRef.current) {
      return;
    }
    if (originalRunning.current) {
      originalPending.current = true;
      return;
    }
    originalRunning.current = true;
    const generation = selectionGeneration.current;
    const started = beginPreviewJob();
    try {
      const maxDimension = previewMaxDimension(
        stateRef.current.zoomMode,
        stateRef.current.imageWidth,
        PREVIEW_MAX_DIMENSION,
      );
      const preview = await client().originalPreview(maxDimension);
      if (generation !== selectionGeneration.current) {
        return;
      }
      dispatch({ type: 'original-preview', preview: previewFromBridge(preview) });
    } catch (error) {
      if (generation !== selectionGeneration.current) {
        return;
      }
      if (error instanceof ModuleCrashedError) {
        sessionRef.current = false;
      }
      if (!(error instanceof CancelledError)) {
        dispatch({ type: 'error', message: messageOf(error) });
      }
    } finally {
      originalRunning.current = false;
      endPreviewJob(started);
      if (originalPending.current) {
        originalPending.current = false;
        // The queued render serves the newest zoom mode.  The caller of the
        // first request reports its own failure; a cancellation of this
        // follow-up request is expected and not shown.
        void renderOriginalPreview().catch((error: unknown) => {
          if (error instanceof ModuleCrashedError) {
            sessionRef.current = false;
          }
          if (!(error instanceof CancelledError)) {
            dispatch({ type: 'error', message: messageOf(error) });
          }
        });
      }
    }
  }, [beginPreviewJob, client, dispatch, endPreviewJob]);

  const openSession = useCallback(async (): Promise<void> => {
    const source = sourceRef.current;
    const itemId = activeItemId.current;
    if (source === null || itemId === null) {
      return;
    }
    const generation = selectionGeneration.current;
    dispatch({
      type: 'activity',
      activity: { state: 'working', message: 'Preparing the image…', fraction: null },
    });
    dispatch({
      type: 'queue-updated',
      id: itemId,
      patch: { status: 'working', message: 'Preparing…' },
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
      if (generation !== selectionGeneration.current) {
        return;
      }
      sessionRef.current = true;
      const baseValues = await client().resolvedSettings(
        overridesToArray({}),
      );
      if (generation !== selectionGeneration.current) {
        return;
      }
      const baseSettings = settingsFromValues(baseValues);
      const metadata = metadataFromBridge(opened.metadata);
      dispatch({
        type: 'source-prepared',
        width: opened.width,
        height: opened.height,
        metadata,
        baseSettings,
      });
      dispatch({
        type: 'queue-updated',
        id: itemId,
        patch: {
          status: 'ready',
          message: null,
          width: opened.width,
          height: opened.height,
          metadata,
          baseSettings,
        },
      });
      await renderOriginalPreview();
      await renderConvertedPreview();
      if (generation !== selectionGeneration.current) {
        return;
      }
      dispatch({
        type: 'activity',
        activity: { state: 'idle', message: 'Ready', fraction: null },
      });
    } catch (error) {
      if (generation !== selectionGeneration.current) {
        return;
      }
      sessionRef.current = false;
      if (error instanceof CancelledError) {
        dispatch({
          type: 'queue-updated',
          id: itemId,
          patch: { status: 'cancelled', message: 'Cancelled' },
        });
        dispatch({
          type: 'activity',
          activity: { state: 'idle', message: 'Cancelled', fraction: null },
        });
        return;
      }
      dispatch({
        type: 'queue-updated',
        id: itemId,
        patch: { status: 'error', message: messageOf(error) },
      });
      dispatch({ type: 'error', message: messageOf(error) });
    } finally {
      if (generation === selectionGeneration.current) {
        sessionLoading.current = false;
      }
    }
  }, [client, renderConvertedPreview, renderOriginalPreview]);

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
    // openSession awaits this call and handles a cancellation itself; when the
    // zoom change triggers it, the rejection is caught here instead.  The
    // CancelledError carries no user-facing error message.
    void renderOriginalPreview().catch((error: unknown) => {
      if (error instanceof ModuleCrashedError) {
        sessionRef.current = false;
      }
      if (!(error instanceof CancelledError)) {
        dispatch({ type: 'error', message: messageOf(error) });
      }
    });
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

  const activateItem = useCallback(
    (id: number | null, announce: boolean) => {
      selectionGeneration.current += 1;
      activeItemId.current = id;
      const files = id === null ? undefined : queueFiles.current.get(id);
      sourceRef.current = files?.source ?? null;
      companionRef.current = files?.companion ?? null;
      sessionRef.current = false;
      if (previewTimer.current !== null) {
        clearTimeout(previewTimer.current);
        previewTimer.current = null;
      }
      previewPending.current = false;
      if (announce) {
        dispatch({ type: 'selection-changed', id });
      }
    },
    [dispatch],
  );

  const addFiles = useCallback(
    (files: File[]) => {
      if (files.length === 0) {
        return;
      }
      const epoch = queueEpoch.current;
      void Promise.all(
        files.map(async (file) => ({ file, bytes: await file.arrayBuffer() })),
      )
        .then((entries) => {
          if (epoch !== queueEpoch.current) {
            return;
          }
          const companions = new Map<string, { file: File; bytes: ArrayBuffer }>();
          const sources: { file: File; bytes: ArrayBuffer }[] = [];
          for (const entry of entries) {
            if (isCompanionName(entry.file.name)) {
              companions.set(fileStem(entry.file.name), entry);
            } else {
              sources.push(entry);
            }
          }
          // An ORI without a matching RAW is a valid source in its own right.
          for (const entry of entries) {
            if (
              isCompanionName(entry.file.name) &&
              !sources.some((source) => fileStem(source.file.name) === fileStem(entry.file.name))
            ) {
              sources.push(entry);
            }
          }

          const newItems: QueueItem[] = [];
          let selectedCompanionChanged = false;
          for (const source of sources) {
            const companion = companions.get(fileStem(source.file.name));
            const companionFile =
              companion === undefined || companion.file.name === source.file.name
                ? null
                : { bytes: companion.bytes, name: companion.file.name };

            // Native AddFiles skips a file name that is already queued; do the
            // same, but attach a newly supplied companion if one is missing.
            const existingId = Array.from(queueFiles.current.entries()).find(
              ([, files]) => files.source.name === source.file.name,
            )?.[0];
            if (existingId !== undefined) {
              const existing = queueFiles.current.get(existingId);
              if (existing !== undefined && companionFile !== null && existing.companion === null) {
                existing.companion = companionFile;
                dispatch({
                  type: 'queue-updated',
                  id: existingId,
                  patch: { companionName: companionFile.name },
                });
                if (activeItemId.current === existingId) {
                  companionRef.current = companionFile;
                  sessionRef.current = false;
                  selectedCompanionChanged = true;
                }
              }
              continue;
            }

            const id = nextItemId.current;
            nextItemId.current += 1;
            queueFiles.current.set(id, {
              source: { bytes: source.bytes, name: source.file.name },
              companion: companionFile,
            });
            newItems.push({
              id,
              sourceName: source.file.name,
              companionName: companionFile?.name ?? null,
              status: 'pending',
              message: null,
              width: 0,
              height: 0,
              metadata: null,
              baseSettings: null,
            });
          }

          if (newItems.length > 0) {
            dispatch({ type: 'queue-added', items: newItems });
          }
          if (activeItemId.current === null && newItems.length > 0) {
            activateItem(newItems[0].id, true);
            void openSession();
          } else if (selectedCompanionChanged) {
            void openSession();
          }
        })
        .catch((error: unknown) => {
          dispatch({ type: 'error', message: messageOf(error) });
        });
    },
    [activateItem, dispatch, openSession],
  );

  const selectQueueItem = useCallback(
    (id: number) => {
      if (convertingRef.current) {
        return;
      }
      if (id === activeItemId.current && sessionRef.current) {
        return;
      }
      if (!queueFiles.current.has(id)) {
        return;
      }
      activateItem(id, true);
      void openSession();
    },
    [activateItem, openSession],
  );

  const removeQueueItem = useCallback(
    (id: number) => {
      if (convertingRef.current) {
        return;
      }
      const currentQueue = stateRef.current.queue;
      const index = currentQueue.findIndex((item) => item.id === id);
      queueFiles.current.delete(id);
      dispatch({ type: 'queue-removed', id });
      if (activeItemId.current !== id) {
        return;
      }
      const remaining = currentQueue.filter((item) => item.id !== id);
      const next = remaining[Math.min(Math.max(index, 0), remaining.length - 1)];
      if (next !== undefined) {
        activateItem(next.id, true);
        void openSession();
      } else {
        activateItem(null, true);
        client().cancel();
      }
    },
    [activateItem, client, dispatch, openSession],
  );

  const clearQueue = useCallback(() => {
    if (convertingRef.current) {
      return;
    }
    queueFiles.current.clear();
    queueEpoch.current += 1;
    activateItem(null, true);
    dispatch({ type: 'queue-cleared' });
    // Terminating the worker also removes the staged session files.
    client().cancel();
  }, [activateItem, client, dispatch]);

  const selectCompanion = useCallback(
    (file: File | null) => {
      const id = activeItemId.current;
      const files = id === null ? undefined : queueFiles.current.get(id);
      if (id === null || files === undefined) {
        return;
      }
      if (file === null) {
        files.companion = null;
        companionRef.current = null;
        dispatch({ type: 'queue-updated', id, patch: { companionName: null } });
        sessionRef.current = false;
        void openSession();
        return;
      }
      void file.arrayBuffer().then((bytes) => {
        if (activeItemId.current !== id || queueFiles.current.get(id) !== files) {
          return;
        }
        const companion = { bytes, name: file.name };
        files.companion = companion;
        companionRef.current = companion;
        dispatch({
          type: 'queue-updated',
          id,
          patch: { companionName: file.name },
        });
        sessionRef.current = false;
        void openSession();
      }).catch((error: unknown) => {
        dispatch({ type: 'error', message: messageOf(error) });
      });
    },
    [dispatch, openSession],
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
        const saved = current.customPresets.find(
          (entry) => entry.name === current.activeCustomPreset,
        );
        if (saved === undefined) {
          return;
        }
        const overrides = makeExplicitOverrides(saved.settings);
        dispatch({
          type: 'preset-applied',
          preset: 'custom',
          settings: saved.settings,
          overrides,
        });
        schedulePreview();
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

  const selectCustomPreset = useCallback(
    (name: string | null) => {
      dispatch({
        type: 'custom-presets-changed',
        presets: stateRef.current.customPresets,
        active: name,
      });
    },
    [dispatch],
  );

  const saveCustomPreset = useCallback(
    (name: string) => {
      const current = stateRef.current;
      const preset = sanitizeCustomPreset({ name, settings: current.settings });
      if (preset === null) {
        dispatch({ type: 'error', message: 'Enter a preset name of 64 characters or fewer.' });
        return;
      }
      const presets = upsertCustomPreset(current.customPresets, preset);
      persistCustomPresets(presets);
      const overrides = makeExplicitOverrides(preset.settings);
      dispatch({ type: 'custom-presets-changed', presets, active: preset.name });
      dispatch({
        type: 'preset-applied',
        preset: 'custom',
        settings: preset.settings,
        overrides,
      });
      schedulePreview();
    },
    [dispatch, schedulePreview],
  );

  const loadCustomPreset = useCallback(
    (name: string) => {
      const current = stateRef.current;
      const preset = current.customPresets.find((entry) => entry.name === name);
      if (preset === undefined) {
        return;
      }
      const overrides = makeExplicitOverrides(preset.settings);
      dispatch({
        type: 'custom-presets-changed',
        presets: current.customPresets,
        active: preset.name,
      });
      dispatch({
        type: 'preset-applied',
        preset: 'custom',
        settings: preset.settings,
        overrides,
      });
      schedulePreview();
    },
    [dispatch, schedulePreview],
  );

  const deleteCustomPreset = useCallback(
    (name: string) => {
      const current = stateRef.current;
      const presets = removeSavedPreset(current.customPresets, name);
      const active =
        current.activeCustomPreset === name ? null : current.activeCustomPreset;
      persistCustomPresets(presets);
      dispatch({ type: 'custom-presets-changed', presets, active });
      if (current.activeCustomPreset === name && current.preset === 'custom') {
        dispatch({
          type: 'settings-changed',
          settings: current.settings,
          overrides: current.overrides,
        });
      }
    },
    [dispatch],
  );

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
    const source = sourceRef.current;
    const snapshot = stateRef.current;
    const items = [...snapshot.queue];
    if (items.length === 0 || source === null) {
      dispatch({ type: 'error', message: 'Load a RAW file first.' });
      return;
    }
    if (convertingRef.current) {
      return;
    }
    convertingRef.current = true;
    if (previewTimer.current !== null) {
      clearTimeout(previewTimer.current);
      previewTimer.current = null;
    }
    previewPending.current = false;
    const overrides = overridesToArray(snapshot.overrides);
    const compression = snapshot.compression;
    let converted = 0;
    let failed = 0;
    let cancelled = false;
    try {
      for (let index = 0; index < items.length; index += 1) {
        const item = items[index];
        const files = queueFiles.current.get(item.id);
        if (files === undefined) {
          failed += 1;
          dispatch({
            type: 'queue-updated',
            id: item.id,
            patch: { status: 'error', message: 'The source file is no longer available.' },
          });
          continue;
        }
        dispatch({
          type: 'activity',
          activity: {
            state: 'working',
            message: `Converting ${index + 1} of ${items.length}: ${item.sourceName}`,
            fraction: index / items.length,
          },
        });
        dispatch({
          type: 'queue-updated',
          id: item.id,
          patch: { status: 'working', message: 'Preparing…' },
        });
        try {
          const opened = await client().open(
            files.source.bytes,
            files.source.name,
            files.companion?.bytes,
            files.companion?.name,
          );
          const baseValues = await client().resolvedSettings(overridesToArray({}));
          const baseSettings = settingsFromValues(baseValues);
          const metadata = metadataFromBridge(opened.metadata);
          dispatch({
            type: 'queue-updated',
            id: item.id,
            patch: {
              status: 'working',
              message: 'Converting…',
              width: opened.width,
              height: opened.height,
              metadata,
              baseSettings,
            },
          });
          const dng = await client().convert(compression, overrides);
          const name = resultNameFor(files.source.name);
          saveFile(new Blob([dng], { type: 'image/x-adobe-dng' }), name);
          converted += 1;
          dispatch({
            type: 'queue-updated',
            id: item.id,
            patch: { status: 'done', message: `Saved ${name}` },
          });
        } catch (error) {
          if (error instanceof CancelledError) {
            cancelled = true;
            dispatch({
              type: 'queue-updated',
              id: item.id,
              patch: { status: 'cancelled', message: 'Cancelled' },
            });
            break;
          }
          if (error instanceof ModuleCrashedError) {
            sessionRef.current = false;
          }
          failed += 1;
          dispatch({
            type: 'queue-updated',
            id: item.id,
            patch: { status: 'error', message: messageOf(error) },
          });
        }
        // Give the browser a moment between automatic downloads; otherwise it
        // may treat the batch as a pop-up/download burst and block the later
        // files.
        await new Promise((resolve) => setTimeout(resolve, 120));
      }
    } finally {
      convertingRef.current = false;
      // The worker now holds the last converted session, not necessarily the
      // selected one.  The next preview action reopens the selected item.
      sessionRef.current = false;
      const message = cancelled
        ? 'Cancelled'
        : failed > 0
          ? `Converted ${converted} of ${items.length} files · ${failed} failed`
          : `Wrote ${converted} DNG file${converted === 1 ? '' : 's'}`;
      dispatch({
        type: 'activity',
        activity: {
          state: cancelled ? 'idle' : failed > 0 ? 'error' : 'done',
          message,
          fraction: cancelled ? null : 1,
        },
      });
    }
  }, [client, dispatch]);

  const cancel = useCallback(() => {
    // The conversion runs inside the module, so the worker is stopped to end
    // it.  The next worker starts without a session, so the image is
    // prepared again before the next conversion.
    if (previewTimer.current !== null) {
      clearTimeout(previewTimer.current);
      previewTimer.current = null;
    }
    previewPending.current = false;
    sessionRef.current = false;
    client().cancel();
  }, [client]);

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
      addFiles,
      selectQueueItem,
      removeQueueItem,
      clearQueue,
      selectCompanion,
      setStageValue,
      applyPreset,
      resetSection,
      selectCustomPreset,
      saveCustomPreset,
      loadCustomPreset,
      deleteCustomPreset,
      convert: () => void convert(),
      cancel,
    }),
    [
      addFiles,
      applyPreset,
      cancel,
      clearQueue,
      convert,
      deleteCustomPreset,
      loadCustomPreset,
      removeQueueItem,
      resetSection,
      saveCustomPreset,
      selectCompanion,
      selectCustomPreset,
      selectQueueItem,
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
