import {
  STAGE_KEYS,
  STAGE_SECTIONS,
  type StageKey,
  type StageSettings,
} from './stageSettings';

export const CUSTOM_PRESET_STORAGE_KEY = 'hiraco.custom-preset.v1';
/** Old web releases kept a named list; migrate its last entry. */
export const LEGACY_CUSTOM_PRESETS_STORAGE_KEY = 'hiraco.custom-presets.v1';

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
 * Validates and clamps a stored custom preset.  A hand-edited or older
 * localStorage entry must not put NaN or out-of-range values into the sliders.
 */
export function sanitizeCustomPreset(value: unknown): StageSettings | null {
  if (value === null || typeof value !== 'object') {
    return null;
  }
  const rawSettings = value as Record<string, unknown>;
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
  return settings as StageSettings;
}

/** Reads the single saved custom preset, migrating the old named list. */
export function loadCustomPreset(
  storage: Storage | null = storageOrNull(),
): StageSettings | null {
  if (storage === null) {
    return null;
  }
  try {
    const text = storage.getItem(CUSTOM_PRESET_STORAGE_KEY);
    if (text !== null) {
      return sanitizeCustomPreset(JSON.parse(text));
    }
    const legacy = storage.getItem(LEGACY_CUSTOM_PRESETS_STORAGE_KEY);
    if (legacy === null) {
      return null;
    }
    const parsed: unknown = JSON.parse(legacy);
    if (!Array.isArray(parsed) || parsed.length === 0) {
      return null;
    }
    const last = parsed[parsed.length - 1] as Record<string, unknown> | null;
    const migrated = last !== null && typeof last === 'object'
        ? sanitizeCustomPreset(last.settings)
        : null;
    if (migrated !== null) {
      persistCustomPreset(migrated, storage);
    } else {
      storage.removeItem(LEGACY_CUSTOM_PRESETS_STORAGE_KEY);
    }
    return migrated;
  } catch {
    return null;
  }
}

/** Persists the one custom preset; storage failures are ignored. */
export function persistCustomPreset(
  settings: StageSettings,
  storage: Storage | null = storageOrNull(),
): void {
  if (storage === null) {
    return;
  }
  try {
    storage.setItem(CUSTOM_PRESET_STORAGE_KEY, JSON.stringify(settings));
    storage.removeItem(LEGACY_CUSTOM_PRESETS_STORAGE_KEY);
  } catch {
    // The UI keeps the preset for the session even when persistence is blocked.
  }
}
