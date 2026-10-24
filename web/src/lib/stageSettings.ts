/**
 * The stage settings model shared by the browser UI and the wasm bridge.
 *
 * The values, ranges and preset math in this file mirror the native wxWidgets
 * GUI (`src/hiraco_gui.cpp`) so that a picture produced in the browser looks
 * exactly like the same picture produced by the native application.
 */

export interface StageSettings {
  stage1_psf_sigma: number;
  stage1_nsr: number;
  stage2_denoise: number;
  stage2_gain1: number;
  stage2_gain2: number;
  stage2_gain3: number;
  stage3_radius: number;
  stage3_gain: number;
}

/** The order the wasm bridge expects the stage values in. */
export const STAGE_KEYS = [
  'stage1_psf_sigma',
  'stage1_nsr',
  'stage2_denoise',
  'stage2_gain1',
  'stage2_gain2',
  'stage2_gain3',
  'stage3_radius',
  'stage3_gain',
] as const;

export type StageKey = (typeof STAGE_KEYS)[number];

/** A partial set of stage values; a missing key means "not overridden". */
export type StageOverrides = Partial<StageSettings>;

/**
 * The defaults of `ResolvedStageSettings` in `src/hiraco_types.h`.
 */
export const DEFAULT_STAGE_SETTINGS: StageSettings = {
  stage1_psf_sigma: 1.0,
  stage1_nsr: 0.09,
  stage2_denoise: 0.33,
  stage2_gain1: 1.6,
  stage2_gain2: 1.3,
  stage2_gain3: 1.1,
  stage3_radius: 2,
  stage3_gain: 1.0,
};

export interface SliderSpec {
  key: StageKey;
  label: string;
  min: number;
  max: number;
  step: number;
  decimals: number;
  integer: boolean;
}

export interface StageSectionSpec {
  id: string;
  title: string;
  description: string;
  sliders: SliderSpec[];
}

/** The three slider sections of the native "Processing" panel. */
export const STAGE_SECTIONS: StageSectionSpec[] = [
  {
    id: 'stage1',
    title: 'Detail Recovery',
    description: 'Recover fine detail from the stacked capture before later refinements.',
    sliders: [
      {
        key: 'stage1_psf_sigma',
        label: 'Blur Radius',
        min: 0.5,
        max: 4.0,
        step: 0.01,
        decimals: 2,
        integer: false,
      },
      {
        key: 'stage1_nsr',
        label: 'Noise Protection',
        min: 0.0,
        max: 0.2,
        step: 0.001,
        decimals: 3,
        integer: false,
      },
    ],
  },
  {
    id: 'stage2',
    title: 'Multi-scale Detail',
    description: 'Balance denoising and sharpening across small to large texture bands.',
    sliders: [
      {
        key: 'stage2_denoise',
        label: 'Denoise',
        min: 0.0,
        max: 1.0,
        step: 0.01,
        decimals: 2,
        integer: false,
      },
      {
        key: 'stage2_gain1',
        label: 'Small Detail',
        min: 0.25,
        max: 4.0,
        step: 0.01,
        decimals: 2,
        integer: false,
      },
      {
        key: 'stage2_gain2',
        label: 'Medium Detail',
        min: 0.25,
        max: 4.0,
        step: 0.01,
        decimals: 2,
        integer: false,
      },
      {
        key: 'stage2_gain3',
        label: 'Large Detail',
        min: 0.25,
        max: 4.0,
        step: 0.01,
        decimals: 2,
        integer: false,
      },
    ],
  },
  {
    id: 'stage3',
    title: 'Edge Refinement',
    description:
      'Protect strong edges while adding local contrast and keeping halos under control.',
    sliders: [
      {
        key: 'stage3_radius',
        label: 'Edge Radius',
        min: 1,
        max: 16,
        step: 1,
        decimals: 0,
        integer: true,
      },
      {
        key: 'stage3_gain',
        label: 'Edge Gain',
        min: 0.0,
        max: 4.0,
        step: 0.01,
        decimals: 2,
        integer: false,
      },
    ],
  },
];

export type PresetId = 'small' | 'medium' | 'strong' | 'custom';

export const PRESET_IDS: PresetId[] = ['small', 'medium', 'strong', 'custom'];

export const PRESET_LABELS: Record<PresetId, string> = {
  small: 'Small',
  medium: 'Medium',
  strong: 'Strong',
  custom: 'Custom',
};

/** The strength each built-in preset applies, from `SettingsForPreset`. */
export const PRESET_STRENGTH: Record<'small' | 'medium' | 'strong', number> = {
  small: 0.6,
  medium: 1.0,
  strong: 1.45,
};

/** float32 rounding, so the arithmetic matches the native float maths. */
const f32 = Math.fround;

export function clamp(value: number, low: number, high: number): number {
  return value < low ? low : value > high ? high : value;
}

/**
 * The native `BuiltInPresetSettings` (`src/hiraco_gui.cpp`).
 */
export function builtInPresetSettings(
  base: StageSettings,
  strength: number,
): StageSettings {
  const s = f32(strength);
  const settings: StageSettings = { ...base };
  const scaleDetail = (value: number) =>
    f32(clamp(f32(f32(1) + f32(f32(value - 1) * s)), 0.25, 4));

  settings.stage1_nsr = f32(
    clamp(f32(f32(base.stage1_nsr) * f32(f32(1.2) - f32(f32(0.2) * s))), 0, 0.2),
  );
  settings.stage2_denoise = f32(
    clamp(f32(f32(base.stage2_denoise) + f32(f32(f32(1) - s) * 0.2)), 0, 1),
  );
  settings.stage2_gain1 = scaleDetail(base.stage2_gain1);
  settings.stage2_gain2 = scaleDetail(base.stage2_gain2);
  settings.stage2_gain3 = scaleDetail(base.stage2_gain3);
  settings.stage3_gain = f32(clamp(f32(f32(base.stage3_gain) * s), 0, 4));

  return settings;
}

/**
 * The native `MakeExplicitStageOverrides`: every value becomes an override.
 */
export function makeExplicitOverrides(settings: StageSettings): StageOverrides {
  const overrides: StageOverrides = {};
  for (const key of STAGE_KEYS) {
    overrides[key] = settings[key];
  }
  return overrides;
}

/**
 * The native `NormalizeStageOverrides`: an override that matches the base
 * value is dropped again, so the core keeps deciding that value itself.
 */
export function normalizeOverrides(
  overrides: StageOverrides,
  base: StageSettings,
): StageOverrides {
  const normalized: StageOverrides = {};
  for (const key of STAGE_KEYS) {
    const value = overrides[key];
    if (value === undefined) {
      continue;
    }
    if (key === 'stage3_radius') {
      if (value !== base[key]) {
        normalized[key] = value;
      }
      continue;
    }
    if (Math.abs(value - base[key]) > 1e-6) {
      normalized[key] = value;
    }
  }
  return normalized;
}

/**
 * The native `SettingsEqual`: compare with the precision the sliders show.
 */
export function settingsEqual(
  left: StageSettings,
  right: StageSettings,
): boolean {
  const equal = (a: number, b: number, scale: number) =>
    Math.round(a * scale) === Math.round(b * scale);
  return (
    equal(left.stage1_psf_sigma, right.stage1_psf_sigma, 100) &&
    equal(left.stage1_nsr, right.stage1_nsr, 1000) &&
    equal(left.stage2_denoise, right.stage2_denoise, 100) &&
    equal(left.stage2_gain1, right.stage2_gain1, 100) &&
    equal(left.stage2_gain2, right.stage2_gain2, 100) &&
    equal(left.stage2_gain3, right.stage2_gain3, 100) &&
    left.stage3_radius === right.stage3_radius &&
    equal(left.stage3_gain, right.stage3_gain, 100)
  );
}

/**
 * Serialise the overrides for the wasm bridge: one double per stage value,
 * `NaN` meaning "not overridden".
 */
export function serializeStageOverrides(overrides: StageOverrides): Float64Array {
  const values = new Float64Array(STAGE_KEYS.length);
  for (let index = 0; index < STAGE_KEYS.length; index += 1) {
    const value = overrides[STAGE_KEYS[index]];
    values[index] = value === undefined ? Number.NaN : value;
  }
  return values;
}

/** Read the overrides back from the bridge representation. */
export function deserializeStageOverrides(
  values: ArrayLike<number>,
): StageOverrides {
  const overrides: StageOverrides = {};
  for (let index = 0; index < STAGE_KEYS.length; index += 1) {
    const value = values[index];
    if (Number.isNaN(value)) {
      continue;
    }
    overrides[STAGE_KEYS[index]] = value;
  }
  return overrides;
}

/** Apply the overrides on top of the base settings. */
export function applyOverrides(
  base: StageSettings,
  overrides: StageOverrides,
): StageSettings {
  const settings: StageSettings = { ...base };
  for (const key of STAGE_KEYS) {
    const value = overrides[key];
    if (value !== undefined) {
      settings[key] = value;
    }
  }
  return settings;
}

/** Format a value the way the native sliders show it. */
export function formatStageValue(spec: SliderSpec, value: number): string {
  return value.toFixed(spec.decimals);
}
