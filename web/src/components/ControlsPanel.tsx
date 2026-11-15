import { useState } from 'react';
import { useAppState } from '../state/AppStateProvider';
import {
  COMPRESSION_MODES,
  type CompressionMode,
} from '../state/appState';
import {
  PRESET_IDS,
  PRESET_LABELS,
  STAGE_SECTIONS,
  formatStageValue,
} from '../lib/stageSettings';

export function ControlsPanel() {
  const { state, dispatch, actions } = useAppState();
  const busy = state.activity.state === 'working';
  const activeCustomPreset =
    state.customPresets.find((preset) => preset.name === state.activeCustomPreset) ??
    null;
  const [presetName, setPresetName] = useState('');

  return (
    <section className="panel" aria-label="Processing">
      <header className="panel-header">
        <h2 className="panel-title">Processing</h2>
      </header>
      <div className="panel-body">
        <div className="section">
          <div className="section-header">
            <h3 className="section-title">Presets</h3>
          </div>
          <div className="preset-grid">
            {PRESET_IDS.map((preset) => (
              <button
                key={preset}
                type="button"
                className="preset-button"
                aria-pressed={state.preset === preset}
                disabled={busy || (preset === 'custom' && activeCustomPreset === null)}
                title={
                  preset === 'custom' && activeCustomPreset === null
                    ? 'Save or select a custom preset first.'
                    : undefined
                }
                onClick={() => actions.applyPreset(preset)}
              >
                {PRESET_LABELS[preset]}
              </button>
            ))}
          </div>
          {state.selectedItemId !== null && (
            <p className="hint preset-hint">
              Applies to the selected file. Use Paste to all files in the queue to
              sync it.
            </p>
          )}

          <div className="field custom-preset-field">
            <label className="field-label" htmlFor="saved-custom-preset">
              Saved custom preset
            </label>
            <select
              id="saved-custom-preset"
              className="select"
              aria-label="Saved custom preset"
              value={state.activeCustomPreset ?? ''}
              disabled={busy}
              onChange={(event) =>
                actions.selectCustomPreset(event.target.value === '' ? null : event.target.value)
              }
            >
              <option value="">None</option>
              {state.customPresets.map((preset) => (
                <option key={preset.name} value={preset.name}>
                  {preset.name}
                </option>
              ))}
            </select>
          </div>
          <div className="button-row custom-preset-actions">
            <button
              type="button"
              className="button"
              disabled={busy || activeCustomPreset === null}
              onClick={() => {
                if (activeCustomPreset !== null) {
                  actions.loadCustomPreset(activeCustomPreset.name);
                }
              }}
            >
              Load
            </button>
            <button
              type="button"
              className="button danger"
              disabled={busy || activeCustomPreset === null}
              onClick={() => {
                if (activeCustomPreset !== null) {
                  actions.deleteCustomPreset(activeCustomPreset.name);
                }
              }}
            >
              Delete
            </button>
          </div>

          <div className="field custom-preset-field">
            <label className="field-label" htmlFor="custom-preset-name">
              Save current settings as
            </label>
            <div className="inline-row">
              <input
                id="custom-preset-name"
                className="text-input"
                type="text"
                aria-label="Custom preset name"
                placeholder="Preset name"
                maxLength={64}
                value={presetName}
                disabled={busy}
                onChange={(event) => setPresetName(event.target.value)}
              />
              <button
                type="button"
                className="button"
                disabled={busy || presetName.trim() === ''}
                onClick={() => {
                  actions.saveCustomPreset(presetName);
                }}
              >
                Save preset
              </button>
            </div>
          </div>
        </div>

        {STAGE_SECTIONS.map((section) => (
          <div className="section" key={section.id}>
            <div className="section-header">
              <h3 className="section-title">{section.title}</h3>
              <button
                type="button"
                className="button ghost"
                onClick={() => actions.resetSection(section.sliders.map((slider) => slider.key))}
              >
                Reset
              </button>
            </div>
            <p className="section-description">{section.description}</p>
            {section.sliders.map((slider) => (
              <div className="slider-row" key={slider.key}>
                <div className="slider-head">
                  <label htmlFor={`slider-${slider.key}`}>{slider.label}</label>
                  <span className="slider-value">
                    {formatStageValue(slider, state.settings[slider.key])}
                  </span>
                </div>
                <input
                  id={`slider-${slider.key}`}
                  className="slider"
                  type="range"
                  min={slider.min}
                  max={slider.max}
                  step={slider.step}
                  value={state.settings[slider.key]}
                  disabled={busy}
                  onChange={(event) => {
                    const raw = Number(event.target.value);
                    actions.setStageValue(
                      slider.key,
                      slider.integer ? Math.round(raw) : raw,
                    );
                  }}
                />
              </div>
            ))}
          </div>
        ))}

        <div className="section">
          <div className="section-header">
            <h3 className="section-title">Output</h3>
          </div>
          <div className="field">
            <span className="field-label">Compression</span>
            <select
              className="select"
              value={state.compression}
              disabled={busy}
              aria-label="Compression"
              onChange={(event) =>
                dispatch({
                  type: 'compression-changed',
                  compression: event.target.value as CompressionMode,
                })
              }
            >
              {COMPRESSION_MODES.map((mode) => (
                <option key={mode.id} value={mode.id}>
                  {mode.label}
                </option>
              ))}
            </select>
          </div>
        </div>
      </div>

      <footer className="panel-footer">
        <div className="button-row">
          <button
            type="button"
            className="button primary"
            disabled={
              busy || state.queue.length === 0 || state.moduleStatus !== 'ready'
            }
            title={
              state.queue.length > 1
                ? `Convert all ${state.queue.length} queued files using their own settings`
                : 'Convert the queued file using its own settings'
            }
            onClick={() => actions.convert()}
          >
            Convert
          </button>
          <button
            type="button"
            className="button danger"
            disabled={!busy}
            title="Stop the running conversion. The worker restarts, so the next conversion prepares the image again."
            onClick={() => actions.cancel()}
          >
            Cancel
          </button>
        </div>
        {state.queue.length > 1 && (
          <p className="hint convert-hint">
            Converts all {state.queue.length} queued files, each with its own settings.
          </p>
        )}
      </footer>
    </section>
  );
}
