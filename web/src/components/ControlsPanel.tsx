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
                onClick={() => actions.applyPreset(preset)}
              >
                {PRESET_LABELS[preset]}
              </button>
            ))}
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
            disabled={busy || state.sourceName === null}
            onClick={() => actions.convert()}
          >
            Convert
          </button>
          <button
            type="button"
            className="button danger"
            disabled={!busy}
            onClick={() => actions.cancel()}
          >
            Cancel
          </button>
          <button
            type="button"
            className="button"
            disabled={state.result === null}
            onClick={() => actions.download()}
          >
            Save DNG
          </button>
        </div>
        {state.resultSummary !== null && (
          <p className="hint" data-testid="writer-summary">
            {state.resultSummary}
          </p>
        )}
      </footer>
    </section>
  );
}
