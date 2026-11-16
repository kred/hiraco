import { describe, expect, it } from 'vitest';
import { PREVIEW_MAX_DIMENSION } from '../lib/compare';
import {
  PREVIEW_CACHE_LIMIT_BYTES,
  QUEUE_PREVIEW_CACHE_LIMIT_BYTES,
  appReducer,
  initialAppState,
  matchedPreset,
  type AppState,
  type PreviewBuffer,
  type QueueItem,
} from './appState';
import {
  DEFAULT_STAGE_SETTINGS,
  applyOverrides,
  builtInPresetSettings,
  PRESET_STRENGTH,
  type StageOverrides,
  type StageSettings,
} from '../lib/stageSettings';

function makeItem(
  id: number,
  overrides: StageOverrides = {},
  base: StageSettings = DEFAULT_STAGE_SETTINGS,
): QueueItem {
  return {
    id,
    sourceName: `image-${id}.ORF`,
    companionName: null,
    status: 'ready',
    message: null,
    width: 6000,
    height: 4000,
    metadata: null,
    baseSettings: base,
    settings: applyOverrides(base, overrides),
    overrides,
    preset: null,
    cachedOriginalPreview: null,
    cachedOriginalMaxDimension: null,
    cachedConvertedPreview: null,
    cachedConvertedMaxDimension: null,
  };
}

function makeState(overrides: Partial<AppState> = {}): AppState {
  return { ...initialAppState, ...overrides };
}

describe('the application reducer', () => {
  it('applies settings edits to the selected queue entry only', () => {
    const first = makeItem(1);
    const second = makeItem(2);
    const state = makeState({
      queue: [first, second],
      selectedItemId: 1,
      sourceName: first.sourceName,
    });

    const next = appReducer(state, {
      type: 'settings-changed',
      settings: { ...DEFAULT_STAGE_SETTINGS, stage1_psf_sigma: 2.5 },
      overrides: { stage1_psf_sigma: 2.5 },
    });

    expect(next.queue[0].settings?.stage1_psf_sigma).toBe(2.5);
    expect(next.queue[0].overrides).toEqual({ stage1_psf_sigma: 2.5 });
    expect(next.queue[1].settings).toEqual(second.settings);
    expect(next.settings.stage1_psf_sigma).toBe(2.5);
  });

  it('keeps the preset badge of every entry value-based after a paste', () => {
    const strong = builtInPresetSettings(DEFAULT_STAGE_SETTINGS, PRESET_STRENGTH.strong);
    const queue = [
      makeItem(1),
      makeItem(2),
      // A file that has not been prepared yet cannot claim a preset.
      { ...makeItem(3), baseSettings: null, settings: null },
    ];
    const state = makeState({ queue, selectedItemId: 1 });
    const value = {
      settings: strong,
      overrides: { ...strong },
    };

    const next = appReducer(state, {
      type: 'settings-pasted',
      ids: [1, 2, 3],
      value,
    });

    expect(next.queue[0].preset).toBe('strong');
    expect(next.queue[1].preset).toBe('strong');
    expect(next.queue[2].preset).toBeNull();
    expect(next.preset).toBe('strong');
  });

  it('recomputes preset badges when the custom slot is overwritten', () => {
    const custom: StageSettings = {
      ...DEFAULT_STAGE_SETTINGS,
      stage1_psf_sigma: 2.5,
    };
    const queue = [
      makeItem(1, { stage1_psf_sigma: 2.5 }),
      makeItem(2, { stage1_psf_sigma: 1.5 }),
    ];
    const state = makeState({
      queue,
      selectedItemId: 1,
      settings: custom,
      baseSettings: DEFAULT_STAGE_SETTINGS,
      customPreset: { ...DEFAULT_STAGE_SETTINGS, stage1_psf_sigma: 1.5 },
      // The stale label a stored preset would have left behind.
      preset: 'custom',
    });

    const next = appReducer(state, {
      type: 'custom-preset-changed',
      settings: custom,
    });

    expect(next.queue[0].preset).toBe('custom');
    expect(next.queue[1].preset).toBeNull();
    expect(next.preset).toBe('custom');
    expect(next.customPreset).toEqual(custom);
  });

  it('caches fit previews per queue row and restores them on reselection', () => {
    const preview = {
      width: 2,
      height: 1,
      pixels: new Uint8ClampedArray(8),
      recoveredMask: new Uint8Array(2),
    };
    const first = makeItem(1);
    const second = makeItem(2);
    const state = makeState({
      queue: [first, second],
      selectedItemId: 1,
      convertedPreview: null,
      originalPreview: null,
    });

    const rendered = appReducer(state, { type: 'converted-preview', preview, maxDimension: PREVIEW_MAX_DIMENSION });
    expect(rendered.queue[0].cachedConvertedPreview).toBe(preview);
    expect(rendered.queue[1].cachedConvertedPreview).toBeNull();

    const switched = appReducer(
      { ...rendered, selectedItemId: 2, convertedPreview: null },
      { type: 'selection-changed', id: 1 },
    );
    expect(switched.convertedPreview).toBe(preview);

    const zoomed = appReducer(
      { ...rendered, selectedItemId: 2, zoomMode: '50', convertedPreview: null },
      { type: 'selection-changed', id: 1 },
    );
    expect(zoomed.convertedPreview).toBeNull();

    const companionChanged = appReducer(rendered, {
      type: 'queue-updated',
      id: 1,
      patch: { companionName: 'image-1.ORI' },
    });
    expect(companionChanged.queue[0].cachedConvertedPreview).toBeNull();
    expect(companionChanged.queue[0].cachedOriginalPreview).toBeNull();
  });

  it('reuses a cached preview when a larger render satisfies the zoom', () => {
    const preview = {
      width: 2,
      height: 1,
      pixels: new Uint8ClampedArray(8),
      recoveredMask: new Uint8Array(2),
    };
    const first = makeItem(1);
    const second = makeItem(2);
    const state = makeState({ queue: [first, second], selectedItemId: 1 });
    const rendered = appReducer(state, {
      type: 'converted-preview',
      preview,
      maxDimension: 5193,
    });
    const switched = appReducer(
      { ...rendered, selectedItemId: 2, zoomMode: '50', convertedPreview: null },
      { type: 'selection-changed', id: 1 },
    );
    expect(switched.convertedPreview).toBe(preview);

    const fullRendered = appReducer(
      makeState({ queue: [first, second], selectedItemId: 1 }),
      { type: 'converted-preview', preview, maxDimension: 0 },
    );
    const fitFromFull = appReducer(
      { ...fullRendered, selectedItemId: 2, zoomMode: 'fit', convertedPreview: null },
      { type: 'selection-changed', id: 1 },
    );
    expect(fitFromFull.convertedPreview).toBe(preview);
  });

  it('evicts old row previews when the queue cache budget is exceeded', () => {
    const fakePreview = (bytes: number) =>
      ({
        width: 1,
        height: 1,
        pixels: { byteLength: bytes },
        recoveredMask: { byteLength: 0 },
      }) as unknown as PreviewBuffer;
    const items = [1, 2, 3, 4].map((id) => makeItem(id));
    let state = makeState({ queue: items, selectedItemId: 1 });
    for (const id of [1, 2, 3, 4]) {
      state = { ...state, selectedItemId: id };
      state = appReducer(state, {
        type: 'converted-preview',
        preview: fakePreview(30 * 1024 * 1024),
        maxDimension: PREVIEW_MAX_DIMENSION,
      });
    }
    const total = state.queue.reduce(
      (sum, item) => sum + (item.cachedConvertedPreview?.pixels.byteLength ?? 0),
      0,
    );
    expect(total).toBeLessThanOrEqual(QUEUE_PREVIEW_CACHE_LIMIT_BYTES);
    expect(state.queue.find((item) => item.id === 4)?.cachedConvertedPreview).not.toBeNull();
  });

  it('counts the recovered mask against the preview cache limit', () => {
    const preview = {
      width: 2,
      height: 1,
      pixels: new Uint8ClampedArray(8),
      recoveredMask: new Uint8Array(PREVIEW_CACHE_LIMIT_BYTES + 1),
    };
    const state = makeState({ queue: [makeItem(1)], selectedItemId: 1 });
    const rendered = appReducer(state, { type: 'converted-preview', preview, maxDimension: PREVIEW_MAX_DIMENSION });
    expect(rendered.convertedPreview).toBe(preview);
    expect(rendered.queue[0].cachedConvertedPreview).toBeNull();
  });

  it('drops a cached converted preview when the values change', () => {
    const preview = {
      width: 2,
      height: 1,
      pixels: new Uint8ClampedArray(8),
      recoveredMask: new Uint8Array(2),
    };
    const state = makeState({
      queue: [makeItem(1), makeItem(2)],
      selectedItemId: 1,
    });
    const rendered = appReducer(state, { type: 'converted-preview', preview, maxDimension: PREVIEW_MAX_DIMENSION });
    const edited = appReducer(rendered, {
      type: 'settings-changed',
      settings: rendered.settings,
      overrides: {},
    });
    expect(edited.queue[0].cachedConvertedPreview).toBeNull();

    const pasted = appReducer(rendered, {
      type: 'settings-pasted',
      ids: [1, 2],
      value: { settings: rendered.settings, overrides: {} },
    });
    expect(pasted.queue.every((item) => item.cachedConvertedPreview === null)).toBe(true);
  });

  it('resets the processing panel when the queue is cleared', () => {
    const state = makeState({
      queue: [makeItem(1)],
      selectedItemId: 1,
      sourceName: 'image-1.ORF',
      settings: { ...DEFAULT_STAGE_SETTINGS, stage1_psf_sigma: 2.5 },
      overrides: { stage1_psf_sigma: 2.5 },
      preset: 'custom',
      convertedPreview: {
        width: 1,
        height: 1,
        pixels: new Uint8ClampedArray(4),
        recoveredMask: new Uint8Array(1),
      },
      customPreset: { ...DEFAULT_STAGE_SETTINGS, stage1_psf_sigma: 2.5 },
    });

    const next = appReducer(state, { type: 'queue-cleared' });

    expect(next.queue).toEqual([]);
    expect(next.selectedItemId).toBeNull();
    expect(next.baseSettings).toEqual(DEFAULT_STAGE_SETTINGS);
    expect(next.settings).toEqual(DEFAULT_STAGE_SETTINGS);
    expect(next.overrides).toEqual({});
    expect(next.preset).toBeNull();
    expect(next.convertedPreview).toBeNull();
    expect(next.customPreset).toEqual(state.customPreset);
  });

  it('uses the defaults for an unprepared entry instead of the previous base', () => {
    const previous = { ...DEFAULT_STAGE_SETTINGS, stage2_denoise: 0.1 };
    const unprepared = { ...makeItem(2), baseSettings: null, settings: null };
    const state = makeState({
      queue: [unprepared],
      selectedItemId: null,
      baseSettings: previous,
      settings: previous,
    });

    const next = appReducer(state, { type: 'selection-changed', id: 2 });

    expect(next.baseSettings).toEqual(DEFAULT_STAGE_SETTINGS);
    expect(next.settings).toEqual(DEFAULT_STAGE_SETTINGS);
  });

  it('tracks the destructive conversion separately from preview work', () => {
    const state = makeState({ activity: { state: 'working', message: 'Rendering preview…', fraction: null } });

    expect(appReducer(state, { type: 'converting', running: true }).converting).toBe(true);
    expect(
      appReducer(
        { ...state, converting: true },
        { type: 'converting', running: false },
      ).converting,
    ).toBe(false);
  });
});

describe('preset matching', () => {
  it('matches a built-in preset from the image base', () => {
    const base = { ...DEFAULT_STAGE_SETTINGS, stage2_denoise: 0.24 };
    const strong = builtInPresetSettings(base, PRESET_STRENGTH.strong);
    expect(matchedPreset(base, strong, null)).toBe('strong');
  });

  it('matches the custom slot only when its values are selected', () => {
    const custom = { ...DEFAULT_STAGE_SETTINGS, stage1_psf_sigma: 2.5 };
    const other = { ...DEFAULT_STAGE_SETTINGS, stage1_psf_sigma: 1.2 };
    expect(matchedPreset(DEFAULT_STAGE_SETTINGS, custom, custom)).toBe('custom');
    expect(matchedPreset(DEFAULT_STAGE_SETTINGS, other, custom)).toBeNull();
  });
});
