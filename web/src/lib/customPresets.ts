import {
  STAGE_KEYS,
  STAGE_SECTIONS,
  type StageKey,
  type StageSettings,
} from './stageSettings';

/** A user-named set of absolute stage values. */
export interface CustomPreset {
  name: string;
  settings: StageSettings;
}

export const CUSTOM_PRESETS_STORAGE_KEY = 'hiraco.custom-presets.v1';

const MAX_NAME_LENGTH = 64;

const SLIDER_BY_KEY = new Map(
  STAGE_SECTIONS.flatMap((section) => section.sliders.map((slider) => [slider.key, slider])),
);

function storageOrNull(): Storage | null {
  try {
    return typeof window === 'undefined' ? null : window.localStorage;
  } catch {
    // Private mode or a blocked storage partition.
    return null;
  }
}

/**
 * Validates and clamps a stored preset.  A hand-edited or older localStorage
 * entry must not put NaN or out-of-range values into the sliders.
 */
export function sanitizeCustomPreset(value: unknown): CustomPreset | null {
  if (value === null || typeof value !== 'object') {
    return null;
  }
  const record = value as Record<string, unknown>;
  const name = typeof record.name === 'string' ? record.name.trim() : '';
  if (name === '' || name.length > MAX_NAME_LENGTH) {
    return null;
  }
  if (record.settings === null || typeof record.settings !== 'object') {
    return null;
  }
  const rawSettings = record.settings as Record<string, unknown>;
  const settings = {} as Record<StageKey, number>;
  for (const key of STAGE_KEYS) {
    const raw = rawSettings[key];
    if (typeof raw !== 'number' || !Number.isFinite(raw)) {
      return null;
    }
    const slider = SLIDER_BY_KEY.get(key);
    if (slider === undefined) {
      return null;
    }
    const clamped = Math.min(Math.max(raw, slider.min), slider.max);
    settings[key] = slider.integer ? Math.round(clamped) : clamped;
  }
  return { name, settings: settings as StageSettings };
}

/** Reads the saved presets, ignoring malformed entries. */
export function loadCustomPresets(storage: Storage | null = storageOrNull()): CustomPreset[] {
  if (storage === null) {
    return [];
  }
  try {
    const text = storage.getItem(CUSTOM_PRESETS_STORAGE_KEY);
    if (text === null) {
      return [];
    }
    const parsed: unknown = JSON.parse(text);
    if (!Array.isArray(parsed)) {
      return [];
    }
    const byName = new Map<string, CustomPreset>();
    for (const entry of parsed) {
      const preset = sanitizeCustomPreset(entry);
      if (preset !== null) {
        byName.set(preset.name, preset);
      }
    }
    return Array.from(byName.values());
  } catch {
    return [];
  }
}

/** Persists the presets; storage failures (quota/private mode) are ignored. */
export function persistCustomPresets(
  presets: CustomPreset[],
  storage: Storage | null = storageOrNull(),
): void {
  if (storage === null) {
    return;
  }
  try {
    storage.setItem(CUSTOM_PRESETS_STORAGE_KEY, JSON.stringify(presets));
  } catch {
    // The UI keeps working for the session even when persistence is blocked.
  }
}

/** Adds a preset, replacing one with the same name. */
export function upsertCustomPreset(
  presets: CustomPreset[],
  preset: CustomPreset,
): CustomPreset[] {
  const index = presets.findIndex((entry) => entry.name === preset.name);
  if (index < 0) {
    return [...presets, preset];
  }
  const next = [...presets];
  next[index] = preset;
  return next;
}

export function removeCustomPreset(
  presets: CustomPreset[],
  name: string,
): CustomPreset[] {
  return presets.filter((entry) => entry.name !== name);
}
