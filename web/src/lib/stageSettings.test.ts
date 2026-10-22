import { describe, expect, it } from 'vitest';
import {
  DEFAULT_STAGE_SETTINGS,
  PRESET_STRENGTH,
  STAGE_KEYS,
  applyOverrides,
  builtInPresetSettings,
  deserializeStageOverrides,
  formatStageValue,
  makeExplicitOverrides,
  normalizeOverrides,
  serializeStageOverrides,
  settingsEqual,
  type StageSettings,
} from './stageSettings';

/**
 * The expected numbers are the output of the native formula, printed by a
 * small C++ program that copies `BuiltInPresetSettings` from
 * `src/hiraco_gui.cpp` verbatim (tmp/preset_check.cpp).
 */
const NATIVE_SMALL: StageSettings = {
  stage1_psf_sigma: 1,
  stage1_nsr: 0.0972000062,
  stage2_denoise: 0.409999996,
  stage2_gain1: 1.36000001,
  stage2_gain2: 1.17999995,
  stage2_gain3: 1.06000006,
  stage3_radius: 2,
  stage3_gain: 0.600000024,
};

const NATIVE_MEDIUM: StageSettings = {
  stage1_psf_sigma: 1,
  stage1_nsr: 0.0900000036,
  stage2_denoise: 0.330000013,
  stage2_gain1: 1.60000002,
  stage2_gain2: 1.29999995,
  stage2_gain3: 1.10000002,
  stage3_radius: 2,
  stage3_gain: 1,
};

const NATIVE_STRONG: StageSettings = {
  stage1_psf_sigma: 1,
  stage1_nsr: 0.081900008,
  stage2_denoise: 0.24000001,
  stage2_gain1: 1.87,
  stage2_gain2: 1.43499994,
  stage2_gain3: 1.14499998,
  stage3_radius: 2,
  stage3_gain: 1.45000005,
};

function expectClose(actual: StageSettings, expected: StageSettings) {
  for (const key of STAGE_KEYS) {
    expect(actual[key]).toBeCloseTo(expected[key], 6);
  }
}

describe('builtInPresetSettings', () => {
  it('matches the native small preset', () => {
    expectClose(
      builtInPresetSettings(DEFAULT_STAGE_SETTINGS, PRESET_STRENGTH.small),
      NATIVE_SMALL,
    );
  });

  it('matches the native medium preset', () => {
    expectClose(
      builtInPresetSettings(DEFAULT_STAGE_SETTINGS, PRESET_STRENGTH.medium),
      NATIVE_MEDIUM,
    );
  });

  it('matches the native strong preset', () => {
    expectClose(
      builtInPresetSettings(DEFAULT_STAGE_SETTINGS, PRESET_STRENGTH.strong),
      NATIVE_STRONG,
    );
  });

  it('keeps the medium preset equal to the base settings', () => {
    expect(
      settingsEqual(
        builtInPresetSettings(DEFAULT_STAGE_SETTINGS, PRESET_STRENGTH.medium),
        DEFAULT_STAGE_SETTINGS,
      ),
    ).toBe(true);
  });

  it('clamps the detail gains into the slider range', () => {
    const base: StageSettings = { ...DEFAULT_STAGE_SETTINGS, stage2_gain1: 4 };
    const result = builtInPresetSettings(base, 2.5);
    expect(result.stage2_gain1).toBeLessThanOrEqual(4);
    expect(result.stage2_gain1).toBeGreaterThanOrEqual(0.25);
  });
});

describe('overrides', () => {
  it('drops an override that matches the base value', () => {
    const overrides = makeExplicitOverrides(DEFAULT_STAGE_SETTINGS);
    const normalized = normalizeOverrides(overrides, DEFAULT_STAGE_SETTINGS);
    expect(normalized).toEqual({});
  });

  it('keeps an override that differs from the base value', () => {
    const overrides = { ...makeExplicitOverrides(DEFAULT_STAGE_SETTINGS), stage2_denoise: 0.5 };
    const normalized = normalizeOverrides(overrides, DEFAULT_STAGE_SETTINGS);
    expect(normalized.stage2_denoise).toBe(0.5);
    expect(normalized.stage2_gain1).toBeUndefined();
  });

  it('applies the overrides on top of the base settings', () => {
    const settings = applyOverrides(DEFAULT_STAGE_SETTINGS, { stage3_gain: 2.5 });
    expect(settings.stage3_gain).toBe(2.5);
    expect(settings.stage2_denoise).toBe(DEFAULT_STAGE_SETTINGS.stage2_denoise);
  });
});

describe('override serialization', () => {
  it('uses NaN for the values that are not overridden', () => {
    const values = serializeStageOverrides({ stage2_denoise: 0.5 });
    expect(values).toHaveLength(STAGE_KEYS.length);
    expect(values[STAGE_KEYS.indexOf('stage2_denoise')]).toBe(0.5);
    expect(Number.isNaN(values[STAGE_KEYS.indexOf('stage1_nsr')])).toBe(true);
  });

  it('round trips through the bridge representation', () => {
    const overrides = { stage1_nsr: 0.11, stage3_radius: 4 };
    const restored = deserializeStageOverrides(serializeStageOverrides(overrides));
    expect(restored).toEqual(overrides);
  });
});

describe('formatStageValue', () => {
  it('uses the number of decimals of the slider', () => {
    expect(
      formatStageValue(
        { key: 'stage1_nsr', label: '', min: 0, max: 0.2, step: 0.001, decimals: 3, integer: false },
        0.0972,
      ),
    ).toBe('0.097');
  });
});
