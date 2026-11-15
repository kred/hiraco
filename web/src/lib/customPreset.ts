import {
  STAGE_KEYS,
  STAGE_SECTIONS,
  type StageKey,
  type StageSettings,
} from './stageSettings';

export const CUSTOM_PRESET_STORAGE_KEY = 'hiraco.custom-preset.v1';
/** Old web releases kept a named list; migrate its last valid entry. */
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
      const current = sanitizeCustomPreset(JSON.parse(text));
      if (current !== null) {
        try {
          // The old named list is no longer used.
          storage.removeItem(LEGACY_CUSTOM_PRESETS_STORAGE_KEY);
        } catch {
          // Read-only storage still has a valid preset to return.
        }
        return current;
      }
      // A corrupt new slot must not hide a valid old list.
      try {
        storage.removeItem(CUSTOM_PRESET_STORAGE_KEY);
      } catch {
        // Read-only storage; the value is ignored either way.
      }
    }
  } catch {
    try {
      storage.removeItem(CUSTOM_PRESET_STORAGE_KEY);
    } catch {
      // The storage may be read-only; fall through to the legacy migration.
    }
  }

  try {
    const legacy = storage.getItem(LEGACY_CUSTOM_PRESETS_STORAGE_KEY);
    if (legacy === null) {
      return null;
    }
    const parsed: unknown = JSON.parse(legacy);
    if (!Array.isArray(parsed)) {
      storage.removeItem(LEGACY_CUSTOM_PRESETS_STORAGE_KEY);
      return null;
    }
    // Take the last valid entry; a malformed tail must not throw away earlier
    // presets.  The old UI appended new names and updated existing ones in
    // place, so the last valid element is the best available migration.
    for (let index = parsed.length - 1; index >= 0; index -= 1) {
      const entry = parsed[index] as Record<string, unknown> | null;
      const migrated = entry !== null && typeof entry === 'object'
          ? sanitizeCustomPreset(entry.settings)
          : null;
      if (migrated !== null) {
        persistCustomPreset(migrated, storage);
        return migrated;
      }
    }
    storage.removeItem(LEGACY_CUSTOM_PRESETS_STORAGE_KEY);
    return null;
  } catch {
    return null;
  }
}

/** Persists the one custom preset; returns false when storage is blocked. */
export function persistCustomPreset(
  settings: StageSettings,
  storage: Storage | null = storageOrNull(),
): boolean {
  if (storage === null) {
    return false;
  }
  try {
    storage.setItem(CUSTOM_PRESET_STORAGE_KEY, JSON.stringify(settings));
    storage.removeItem(LEGACY_CUSTOM_PRESETS_STORAGE_KEY);
    return true;
  } catch {
    // The UI keeps the preset for the session even when persistence is blocked.
    return false;
  }
}
