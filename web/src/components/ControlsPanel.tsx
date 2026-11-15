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
  const converting = state.converting;
  const selectedItem = state.queue.find((item) => item.id === state.selectedItemId);
  // A queue entry is editable once its row exists.  While the module is
  // preparing it, the base values are still unknown, so the controls wait.
  // Preview rendering and an already converted row do not lock them.
  const settingsDisabled =
    converting ||
    selectedItem === undefined ||
    selectedItem.status === 'working' ||
    selectedItem.status === 'pending';
  const hasCustomPreset = state.customPreset !== null;

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
          <p className="hint editing-hint">
            {selectedItem === undefined
              ? 'Select a queued file to edit its settings.'
              : (
                  <>
                    Editing <strong>{selectedItem.sourceName}</strong>. Presets and
                    sliders only affect the selected file; use the queue footer to
                    copy or paste settings.
                  </>
                )}
          </p>
          <div className="preset-grid">
            {PRESET_IDS.map((preset) => (
              <button
                key={preset}
                type="button"
                className="preset-button"
                aria-pressed={state.preset === preset}
                disabled={
                  settingsDisabled || (preset === 'custom' && !hasCustomPreset)
                }
                title={
                  preset === 'custom' && !hasCustomPreset
                    ? 'Set a custom preset from the queue first.'
                    : undefined
                }
                onClick={() => actions.applyPreset(preset)}
              >
                {PRESET_LABELS[preset]}
              </button>
            ))}
          </div>
          {!hasCustomPreset && (
            <p className="hint">
              The Custom slot is empty. Save the selected file's settings with the
              bookmark button in the queue footer.
            </p>
          )}
        </div>

        {STAGE_SECTIONS.map((section) => (
          <div className="section" key={section.id}>
            <div className="section-header">
              <h3 className="section-title">{section.title}</h3>
              <button
                type="button"
                className="button ghost"
                disabled={settingsDisabled}
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
                  disabled={settingsDisabled}
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
              disabled={converting}
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
              busy ||
              converting ||
              state.queue.length === 0 ||
              state.moduleStatus !== 'ready'
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
            disabled={!converting}
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
