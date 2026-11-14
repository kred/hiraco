import { describe, expect, it } from 'vitest';
import {
  CUSTOM_PRESETS_STORAGE_KEY,
  loadCustomPresets,
  persistCustomPresets,
  removeCustomPreset,
  sanitizeCustomPreset,
  upsertCustomPreset,
  type CustomPreset,
} from './customPresets';
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

const PRESET: CustomPreset = {
  name: 'Portrait',
  settings: { ...DEFAULT_STAGE_SETTINGS, stage1_psf_sigma: 2.5 },
};

describe('custom presets', () => {
  it('round-trips presets through storage', () => {
    const storage = makeStorage();
    persistCustomPresets([PRESET], storage);
    expect(loadCustomPresets(storage)).toEqual([PRESET]);
  });

  it('ignores malformed JSON and malformed entries', () => {
    const storage = makeStorage({
      [CUSTOM_PRESETS_STORAGE_KEY]: JSON.stringify([
        null,
        { name: '', settings: DEFAULT_STAGE_SETTINGS },
        { name: 'Broken', settings: { stage1_psf_sigma: '2' } },
        PRESET,
      ]),
    });
    expect(loadCustomPresets(storage)).toEqual([PRESET]);
    storage.setItem(CUSTOM_PRESETS_STORAGE_KEY, '{not json');
    expect(loadCustomPresets(storage)).toEqual([]);
  });

  it('clamps values to the slider ranges and rounds integers', () => {
    const preset = sanitizeCustomPreset({
      name: '  Wide  ',
      settings: {
        ...DEFAULT_STAGE_SETTINGS,
        stage1_psf_sigma: 99,
        stage2_gain1: -3,
        stage3_radius: 4.7,
      },
    });
    expect(preset?.name).toBe('Wide');
    expect(preset?.settings.stage1_psf_sigma).toBe(4);
    expect(preset?.settings.stage2_gain1).toBe(0.25);
    expect(preset?.settings.stage3_radius).toBe(5);
  });

  it('replaces same-named presets and removes by name', () => {
    const updated: CustomPreset = {
      ...PRESET,
      settings: { ...PRESET.settings, stage2_denoise: 0.5 },
    };
    const presets = upsertCustomPreset([PRESET], updated);
    expect(presets).toHaveLength(1);
    expect(presets[0].settings.stage2_denoise).toBe(0.5);
    expect(removeCustomPreset(presets, 'Portrait')).toEqual([]);
  });

  it('does not throw when storage is blocked', () => {
    const storage = {
      getItem: () => {
        throw new Error('blocked');
      },
      setItem: () => {
        throw new Error('blocked');
      },
    } as unknown as Storage;
    expect(loadCustomPresets(storage)).toEqual([]);
    expect(() => persistCustomPresets([PRESET], storage)).not.toThrow();
  });
});
