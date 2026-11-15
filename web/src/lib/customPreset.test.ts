import { describe, expect, it } from 'vitest';
import {
  CUSTOM_PRESET_STORAGE_KEY,
  LEGACY_CUSTOM_PRESETS_STORAGE_KEY,
  loadCustomPreset,
  persistCustomPreset,
  sanitizeCustomPreset,
} from './customPreset';
import { DEFAULT_STAGE_SETTINGS } from './stageSettings';

function makeStorage(initial: Record<string, string> = {}): Storage {
  const values = new Map(Object.entries(initial));
  return {
    get length() {
      return values.size;
    },
    clear: () => {
      values.clear();
    },
    getItem: (key) => values.get(key) ?? null,
    key: (index) => Array.from(values.keys())[index] ?? null,
    removeItem: (key) => {
      values.delete(key);
    },
    setItem: (key, value) => {
      values.set(key, value);
    },
  };
}

const PRESET = { ...DEFAULT_STAGE_SETTINGS, stage1_psf_sigma: 2.5 };

describe('custom preset', () => {
  it('uses stable storage keys', () => {
    expect(CUSTOM_PRESET_STORAGE_KEY).toBe('hiraco.custom-preset.v1');
    expect(LEGACY_CUSTOM_PRESETS_STORAGE_KEY).toBe('hiraco.custom-presets.v1');
  });

  it('round-trips the single preset through storage', () => {
    const storage = makeStorage();
    expect(persistCustomPreset(PRESET, storage)).toBe(true);
    expect(loadCustomPreset(storage)).toEqual(PRESET);
  });

  it('ignores malformed data', () => {
    const storage = makeStorage({
      [CUSTOM_PRESET_STORAGE_KEY]: JSON.stringify({ stage1_psf_sigma: '2' }),
    });
    expect(loadCustomPreset(storage)).toBeNull();
    storage.setItem(CUSTOM_PRESET_STORAGE_KEY, '{not json');
    expect(loadCustomPreset(storage)).toBeNull();
  });

  it('clamps values to the slider ranges and rounds integers', () => {
    const preset = sanitizeCustomPreset({
      ...DEFAULT_STAGE_SETTINGS,
      stage1_psf_sigma: 99,
      stage2_gain1: -3,
      stage3_radius: 4.7,
    });
    expect(preset?.stage1_psf_sigma).toBe(4);
    expect(preset?.stage2_gain1).toBe(0.25);
    expect(preset?.stage3_radius).toBe(5);
  });

  it('migrates the last valid entry of the old named-preset list', () => {
    const storage = makeStorage({
      'hiraco.custom-presets.v1': JSON.stringify([
        { name: 'Old', settings: { ...PRESET, stage1_psf_sigma: 1.2 } },
        { name: 'Latest', settings: PRESET },
        { name: 'Broken', settings: { stage1_psf_sigma: 'bad' } },
      ]),
    });
    expect(loadCustomPreset(storage)).toEqual(PRESET);
    expect(storage.getItem('hiraco.custom-preset.v1')).toBe(JSON.stringify(PRESET));
    expect(storage.getItem('hiraco.custom-presets.v1')).toBeNull();
  });

  it('falls back to a valid legacy list when the new slot is corrupt', () => {
    const storage = makeStorage({
      'hiraco.custom-preset.v1': '{not json',
      'hiraco.custom-presets.v1': JSON.stringify([
        { name: 'Old', settings: PRESET },
      ]),
    });
    expect(loadCustomPreset(storage)).toEqual(PRESET);
  });

  it('does not throw when storage is blocked', () => {
    const storage = {
      getItem: () => {
        throw new Error('blocked');
      },
      setItem: () => {
        throw new Error('blocked');
      },
      removeItem: () => {
        throw new Error('blocked');
      },
    } as unknown as Storage;
    expect(loadCustomPreset(storage)).toBeNull();
    expect(persistCustomPreset(PRESET, storage)).toBe(false);
  });
});
