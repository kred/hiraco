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
  appReducer,
  matchedPreset,
  initialAppState,
  type AppAction,
  type AppState,
  type PreviewBuffer,
  type QueueItem,
  type SourceMetadata,
} from './appState';
import {
  PREVIEW_MAX_DIMENSION,
  previewDimensionSatisfies,
  previewMaxDimension,
} from '../lib/compare';
import {
  loadCustomPreset,
  persistCustomPreset,
  sanitizeCustomPreset,
} from '../lib/customPreset';
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
  serializeStageOverrides,
  type PresetId,
  type StageKey,
  type StageOverrides,
  type StageSettings,
} from '../lib/stageSettings';

/** How long the browser waits for the sliders to settle before it renders. */
const PREVIEW_DELAY_MS = 250;

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

/** Identifies the inputs of a completed/started preview render. */
interface PreviewRenderStamp {
  itemId: number | null;
  generation: number;
  maxDimension: number;
  overridesKey: string;
}

function stageOverridesKey(overrides: StageOverrides): string {
  return overridesToArray(overrides).join(',');
}

function stampMatches(
  stamp: PreviewRenderStamp | null,
  expected: PreviewRenderStamp,
): boolean {
  return stamp !== null &&
      stamp.itemId === expected.itemId &&
      stamp.generation === expected.generation &&
      previewDimensionSatisfies(stamp.maxDimension, expected.maxDimension) &&
      stamp.overridesKey === expected.overridesKey;
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
  /** Saves the selected file's current settings as the one custom preset. */
  setCustomPreset(): void;
  /** Copies the selected item's settings for a later paste. */
  copySettings(): void;
  /** Applies the copied settings to the selected queue entry. */
  pasteSettings(): void;
  /** Applies the copied settings to every queue entry. */
  pasteSettingsToAll(): void;
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
    appReducer,
    initialAppState,
    (initial) => ({ ...initial, customPreset: loadCustomPreset() }),
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
  // Dimensions of the active item, including the freshly opened size before
  // the source-prepared reducer has rendered.
  const activeSize = useRef({ width: 0, height: 0 });
  // Incremented by Clear all so an in-flight add cannot repopulate the queue.
  const queueEpoch = useRef(0);
  // Incremented whenever the selected queue entry changes; late preview/open
  // replies from the previous entry are discarded instead of overwriting it.
  const selectionGeneration = useRef(0);
  const convertingRef = useRef(false);
  // Set by Cancel so the conversion loop can stop even while it waits between
  // downloads, when there is no in-flight worker request to reject.
  const cancelRequested = useRef(false);

  const previewTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  // Inputs of the most recent preview requests.  A mode/zoom change can reuse
  // a buffer that is already on screen instead of re-running the pipeline.
  const lastConvertedRender = useRef<PreviewRenderStamp | null>(null);
  const lastOriginalRender = useRef<PreviewRenderStamp | null>(null);
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
    if (
      previewJobs.current === 0 &&
      !sessionLoading.current &&
      !convertingRef.current
    ) {
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
    const activeItem = stateRef.current.queue.find(
      (item) => item.id === activeItemId.current,
    );
    const overrides = activeItem?.overrides ?? stateRef.current.overrides;
    const maxDimension = previewMaxDimension(
      stateRef.current.zoomMode,
      activeSize.current.width,
      PREVIEW_MAX_DIMENSION,
    );
    const stamp: PreviewRenderStamp = {
      itemId: activeItemId.current,
      generation,
      maxDimension,
      overridesKey: stageOverridesKey(overrides),
    };
    lastConvertedRender.current = stamp;
    try {
      const preview = await client().convertedPreview(
        maxDimension,
        overridesToArray(overrides),
      );
      if (generation !== selectionGeneration.current) {
        return;
      }
      dispatch({
        type: 'converted-preview',
        preview: previewFromBridge(preview),
        maxDimension,
      });
    } catch (error) {
      if (lastConvertedRender.current === stamp) {
        lastConvertedRender.current = null;
      }
      if (generation !== selectionGeneration.current) {
        return;
      }
      if (error instanceof ModuleCrashedError) {
        sessionRef.current = false;
      }
      // A conversion owns the worker now; a stale preview error must not
      // replace its progress message.
      if (!(error instanceof CancelledError) && !convertingRef.current) {
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
    const maxDimension = previewMaxDimension(
      stateRef.current.zoomMode,
      activeSize.current.width,
      PREVIEW_MAX_DIMENSION,
    );
    const stamp: PreviewRenderStamp = {
      itemId: activeItemId.current,
      generation,
      maxDimension,
      overridesKey: '',
    };
    lastOriginalRender.current = stamp;
    try {
      const preview = await client().originalPreview(maxDimension);
      if (generation !== selectionGeneration.current) {
        return;
      }
      dispatch({
        type: 'original-preview',
        preview: previewFromBridge(preview),
        maxDimension,
      });
    } catch (error) {
      if (lastOriginalRender.current === stamp) {
        lastOriginalRender.current = null;
      }
      if (generation !== selectionGeneration.current) {
        return;
      }
      if (error instanceof ModuleCrashedError) {
        sessionRef.current = false;
      }
      if (!(error instanceof CancelledError) && !convertingRef.current) {
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
    if (convertingRef.current) {
      return;
    }
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
      if (stateRef.current.moduleStatus !== 'ready') {
        dispatch({ type: 'module-status', status: 'ready', message: null });
      }
      sessionRef.current = true;
      activeSize.current = { width: opened.width, height: opened.height };
      const baseValues = await client().resolvedSettings(
        overridesToArray({}),
      );
      if (generation !== selectionGeneration.current) {
        return;
      }
      const baseSettings = settingsFromValues(baseValues);
      const metadata = metadataFromBridge(opened.metadata);
      const queueItem = stateRef.current.queue.find((item) => item.id === itemId);
      const overrides = queueItem?.overrides ?? {};
      const settings = applyOverrides(baseSettings, overrides);
      const preset = matchedPreset(
        baseSettings,
        settings,
        stateRef.current.customPreset,
      );
      dispatch({
        type: 'source-prepared',
        width: opened.width,
        height: opened.height,
        metadata,
        baseSettings,
        settings,
        overrides,
        preset,
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
          settings,
          overrides,
          preset,
        },
      });
      // Render only what the current comparison mode displays.  The original
      // preview is a second full decode of the RAW and is the most expensive
      // step of opening a file; the default Converted view must not pay for it.
      // A cached fit preview already on screen is reused when the row is
      // selected again, so switching back does not decode it a second time.
      const compareMode = stateRef.current.compareMode;
      const requiredPreviewDimension = previewMaxDimension(
        stateRef.current.zoomMode,
        activeSize.current.width,
        PREVIEW_MAX_DIMENSION,
      );
      const cachedOriginalMaxDimension = queueItem?.cachedOriginalMaxDimension ?? null;
      const cachedConvertedMaxDimension = queueItem?.cachedConvertedMaxDimension ?? null;
      const cachedOriginal =
        queueItem?.cachedOriginalPreview !== null &&
        queueItem?.cachedOriginalPreview !== undefined &&
        cachedOriginalMaxDimension !== null &&
        previewDimensionSatisfies(cachedOriginalMaxDimension, requiredPreviewDimension)
          ? queueItem.cachedOriginalPreview
          : null;
      const cachedConverted =
        queueItem?.cachedConvertedPreview !== null &&
        queueItem?.cachedConvertedPreview !== undefined &&
        cachedConvertedMaxDimension !== null &&
        previewDimensionSatisfies(cachedConvertedMaxDimension, requiredPreviewDimension)
          ? queueItem.cachedConvertedPreview
          : null;
      const cachedOverridesKey = stageOverridesKey(queueItem?.overrides ?? {});
      if (compareMode === 'original') {
        if (cachedOriginal !== null) {
          lastOriginalRender.current = {
            itemId,
            generation,
            maxDimension: cachedOriginalMaxDimension ?? requiredPreviewDimension,
            overridesKey: '',
          };
        }
        if (cachedOriginal === null) {
          await renderOriginalPreview();
        }
      } else {
        if (cachedConverted === null) {
          await renderConvertedPreview();
        } else {
          lastConvertedRender.current = {
            itemId,
            generation,
            maxDimension: cachedConvertedMaxDimension ?? requiredPreviewDimension,
            overridesKey: cachedOverridesKey,
          };
        }
        if (compareMode === 'side-by-side' && cachedOriginal === null) {
          await renderOriginalPreview();
        } else if (compareMode === 'side-by-side' && cachedOriginal !== null) {
          lastOriginalRender.current = {
            itemId,
            generation,
            maxDimension: cachedOriginalMaxDimension ?? requiredPreviewDimension,
            overridesKey: '',
          };
        }
      }
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
      } else {
        // The open was superseded by a selection change, a removal or a
        // clear.  Do not leave the abandoned row spinning forever.
        const item = stateRef.current.queue.find((entry) => entry.id === itemId);
        if (item !== undefined && item.status === 'working') {
          dispatch({
            type: 'queue-updated',
            id: itemId,
            patch: {
              status: item.settings === null ? 'pending' : 'ready',
              message: null,
            },
          });
        }
      }
    }
  }, [client, renderConvertedPreview, renderOriginalPreview]);

  const schedulePreview = useCallback(() => {
    if (convertingRef.current || stateRef.current.compareMode === 'original') {
      return;
    }
    if (previewTimer.current !== null) {
      clearTimeout(previewTimer.current);
    }
    previewTimer.current = setTimeout(() => {
      previewTimer.current = null;
      if (convertingRef.current || stateRef.current.compareMode === 'original') {
        return;
      }
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
        // Cancelling a preview or conversion stops the worker, which rejects
        // an in-flight init.  That is not a browser capability problem; the
        // next operation recreates the worker and marks the module ready.
        if (!cancelled && !(error instanceof CancelledError)) {
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

  // Render only previews whose displayed resolution/settings changed.  The
  // opening path renders the visible mode; compare-mode toggles and repeated
  // zoom changes must not rerun a preview that is already on screen.
  useEffect(() => {
    if (state.converting || convertingRef.current) {
      return;
    }
    const item = state.queue.find((entry) => entry.id === state.selectedItemId);
    const maxDimension = previewMaxDimension(
      state.zoomMode,
      state.imageWidth,
      PREVIEW_MAX_DIMENSION,
    );
    const itemId = activeItemId.current;
    const generation = selectionGeneration.current;
    const overrides = item?.overrides ?? state.overrides;
    const convertedExpected: PreviewRenderStamp = {
      itemId,
      generation,
      maxDimension,
      overridesKey: stageOverridesKey(overrides),
    };
    const originalExpected: PreviewRenderStamp = {
      itemId,
      generation,
      maxDimension,
      overridesKey: '',
    };
    const originalNeeded =
      state.compareMode !== 'converted' &&
      !stampMatches(lastOriginalRender.current, originalExpected);
    const convertedNeeded =
      state.compareMode !== 'original' &&
      !stampMatches(lastConvertedRender.current, convertedExpected);

    if (!sessionRef.current) {
      if ((originalNeeded || convertedNeeded) &&
          !sessionLoading.current &&
          state.selectedItemId !== null) {
        void openSession();
      }
      return;
    }
    if (originalNeeded) {
      void renderOriginalPreview().catch((error: unknown) => {
        if (error instanceof ModuleCrashedError) {
          sessionRef.current = false;
        }
        if (!(error instanceof CancelledError)) {
          dispatch({ type: 'error', message: messageOf(error) });
        }
      });
    }
    if (convertedNeeded) {
      void renderConvertedPreview();
    }
  }, [
    state.compareMode,
    state.zoomMode,
    state.converting,
    state.imageWidth,
    state.selectedItemId,
    openSession,
    renderOriginalPreview,
    renderConvertedPreview,
    dispatch,
  ]);

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
      const item = id === null
          ? undefined
          : stateRef.current.queue.find((entry) => entry.id === id);
      activeSize.current = { width: item?.width ?? 0, height: item?.height ?? 0 };
      sessionRef.current = false;
      if (previewTimer.current !== null) {
        clearTimeout(previewTimer.current);
        previewTimer.current = null;
      }
      previewPending.current = false;
      originalPending.current = false;
      sessionLoading.current = false;
      if (announce) {
        dispatch({ type: 'selection-changed', id });
      }
    },
    [dispatch],
  );

  const addFiles = useCallback(
    (files: File[]) => {
      if (files.length === 0 || convertingRef.current) {
        return;
      }
      const epoch = queueEpoch.current;
      // Read one file at a time: parallel reads of several 80 MP ORFs
      // multiply the transient peak memory for no practical gain.
      const readEntries = async (): Promise<
        { file: File; bytes: ArrayBuffer }[]
      > => {
        const entries: { file: File; bytes: ArrayBuffer }[] = [];
        for (const file of files) {
          if (epoch !== queueEpoch.current || convertingRef.current) {
            return [];
          }
          entries.push({ file, bytes: await file.arrayBuffer() });
        }
        return entries;
      };
      void readEntries()
        .then((entries) => {
          if (entries.length === 0 ||
              epoch !== queueEpoch.current ||
              convertingRef.current) {
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
              settings: null,
              overrides: {},
              preset: null,
              cachedOriginalPreview: null,
              cachedOriginalMaxDimension: null,
              cachedConvertedPreview: null,
              cachedConvertedMaxDimension: null,
            });
          }

          if (newItems.length > 0) {
            dispatch({ type: 'queue-added', items: newItems });
          }
          if (activeItemId.current === null && newItems.length > 0) {
            activateItem(newItems[0].id, true);
            void openSession();
          } else if (selectedCompanionChanged && !convertingRef.current) {
            selectionGeneration.current += 1;
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
      if (convertingRef.current) {
        return;
      }
      const id = activeItemId.current;
      const files = id === null ? undefined : queueFiles.current.get(id);
      if (id === null || files === undefined) {
        return;
      }
      if (file === null) {
        files.companion = null;
        companionRef.current = null;
        selectionGeneration.current += 1;
        dispatch({ type: 'queue-updated', id, patch: { companionName: null } });
        sessionRef.current = false;
        void openSession();
        return;
      }
      void file.arrayBuffer().then((bytes) => {
        if (convertingRef.current ||
            activeItemId.current !== id ||
            queueFiles.current.get(id) !== files) {
          return;
        }
        const companion = { bytes, name: file.name };
        files.companion = companion;
        companionRef.current = companion;
        selectionGeneration.current += 1;
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
      if (current.selectedItemId === null || convertingRef.current) {
        return;
      }
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
      if (current.selectedItemId === null || convertingRef.current) {
        return;
      }
      if (preset === 'custom') {
        const saved = current.customPreset;
        if (saved === null) {
          return;
        }
        const overrides = makeExplicitOverrides(saved);
        dispatch({
          type: 'preset-applied',
          preset: 'custom',
          settings: saved,
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

  const setCustomPreset = useCallback(() => {
    const current = stateRef.current;
    if (current.selectedItemId === null || convertingRef.current) {
      return;
    }
    const settings = sanitizeCustomPreset(current.settings);
    if (settings === null) {
      dispatch({ type: 'error', message: 'The current settings cannot be saved as a custom preset.' });
      return;
    }
    const persisted = persistCustomPreset(settings);
    // The reducer re-derives the preset badge of every row from its values, so
    // a custom preset that matches this row highlights it without storing a
    // stale label.
    dispatch({ type: 'custom-preset-changed', settings });
    dispatch({
      type: 'activity',
      activity: {
        state: 'done',
        message: persisted
          ? 'Saved the custom preset'
          : 'Saved the custom preset for this session; browser storage is unavailable',
        fraction: null,
      },
    });
  }, [dispatch]);

  const copySettings = useCallback(() => {
    const current = stateRef.current;
    if (current.selectedItemId === null || convertingRef.current) {
      return;
    }
    dispatch({
      type: 'settings-copied',
      value: {
        settings: current.settings,
        // Copy every value explicitly, like the native "Copy settings": a
        // paste must not fall back to different per-image base values for
        // keys the source happened to leave at its base.
        overrides: makeExplicitOverrides(current.settings),
      },
    });
    dispatch({
      type: 'activity',
      activity: {
        state: 'done',
        message: `Copied settings from ${current.sourceName ?? 'the selected file'}`,
        fraction: null,
      },
    });
  }, [dispatch]);

  const pasteSettingsTo = useCallback(
    (all: boolean) => {
      const current = stateRef.current;
      const value = current.copiedSettings;
      if (value === null || convertingRef.current) {
        return;
      }
      const ids = all
        ? current.queue.map((item) => item.id)
        : current.selectedItemId === null
          ? []
          : [current.selectedItemId];
      if (ids.length === 0) {
        return;
      }
      dispatch({ type: 'settings-pasted', ids, value });
      dispatch({
        type: 'activity',
        activity: {
          state: 'done',
          message: `Pasted settings to ${ids.length} file${ids.length === 1 ? '' : 's'}`,
          fraction: null,
        },
      });
      schedulePreview();
    },
    [dispatch, schedulePreview],
  );

  const pasteSettings = useCallback(() => pasteSettingsTo(false), [pasteSettingsTo]);
  const pasteSettingsToAll = useCallback(() => pasteSettingsTo(true), [pasteSettingsTo]);

  const resetSection = useCallback(
    (keys: StageKey[]) => {
      const current = stateRef.current;
      if (current.selectedItemId === null || convertingRef.current) {
        return;
      }
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
    const snapshot = stateRef.current;
    const items = [...snapshot.queue];
    if (items.length === 0 || convertingRef.current || sessionLoading.current) {
      return;
    }
    convertingRef.current = true;
    cancelRequested.current = false;
    originalPending.current = false;
    dispatch({ type: 'converting', running: true });
    if (previewTimer.current !== null) {
      clearTimeout(previewTimer.current);
      previewTimer.current = null;
    }
    previewPending.current = false;
    const compression = snapshot.compression;
    let converted = 0;
    let failed = 0;
    let cancelled = false;
    // The worker may already hold the selected item's prepared source.  Reuse
    // it for the first conversion instead of opening the RAW a second time.
    let sessionItemId: number | null = sessionRef.current ? activeItemId.current : null;
    try {
      for (let index = 0; index < items.length; index += 1) {
        if (cancelRequested.current) {
          cancelled = true;
          break;
        }
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
          const reuseSession =
            item.id === sessionItemId &&
            item.baseSettings !== null &&
            item.settings !== null &&
            item.metadata !== null;
          let baseSettings = item.baseSettings;
          let settings = item.settings;
          let metadata = item.metadata;
          let width = item.width;
          let height = item.height;
          if (!reuseSession) {
            const opened = await client().open(
              files.source.bytes,
              files.source.name,
              files.companion?.bytes,
              files.companion?.name,
            );
            sessionItemId = item.id;
            if (snapshot.moduleStatus !== 'ready') {
              dispatch({ type: 'module-status', status: 'ready', message: null });
            }
            const baseValues = await client().resolvedSettings(overridesToArray({}));
            baseSettings = settingsFromValues(baseValues);
            metadata = metadataFromBridge(opened.metadata);
            settings = applyOverrides(baseSettings, item.overrides);
            width = opened.width;
            height = opened.height;
          }
          const preset = matchedPreset(
            baseSettings!,
            settings!,
            snapshot.customPreset,
          );
          dispatch({
            type: 'queue-updated',
            id: item.id,
            patch: {
              status: 'working',
              message: 'Converting…',
              width,
              height,
              metadata,
              baseSettings,
              settings,
              overrides: item.overrides,
              preset,
            },
          });
          const dng = await client().convert(
            compression,
            overridesToArray(item.overrides),
          );
          if (cancelRequested.current) {
            cancelled = true;
            dispatch({
              type: 'queue-updated',
              id: item.id,
              patch: { status: 'cancelled', message: 'Cancelled' },
            });
            break;
          }
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
            sessionItemId = null;
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
      if (cancelRequested.current) {
        cancelled = true;
      }
    } finally {
      convertingRef.current = false;
      cancelRequested.current = false;
      dispatch({ type: 'converting', running: false });
      // Keep the prepared session only when it still belongs to the selected
      // item; otherwise the next preview action reopens the row.
      sessionRef.current = !cancelled && sessionItemId === activeItemId.current;
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
    originalPending.current = false;
    cancelRequested.current = true;
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
      setCustomPreset,
      copySettings,
      pasteSettings,
      pasteSettingsToAll,
      convert: () => void convert(),
      cancel,
    }),
    [
      addFiles,
      applyPreset,
      cancel,
      clearQueue,
      convert,
      copySettings,
      pasteSettings,
      pasteSettingsToAll,
      removeQueueItem,
      resetSection,
      setCustomPreset,
      selectCompanion,
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
