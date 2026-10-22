import { useEffect, useMemo, useRef } from 'react';
import { COMPARE_MODES, ZOOM_MODES, zoomFactor } from '../lib/compare';
import {
  applyRecoveredMask,
  composeReveal,
  composeSideBySide,
  toImageData,
  type PixelBuffer,
} from '../lib/previewCanvas';
import { useAppState } from '../state/AppStateProvider';

export function PreviewPanel() {
  const { state, dispatch } = useAppState();
  const canvasRef = useRef<HTMLCanvasElement>(null);

  const original = state.originalPreview;
  const converted = useMemo(
    () =>
      state.convertedPreview
        ? applyRecoveredMask(state.convertedPreview, state.showRecoveredPixels)
        : null,
    [state.convertedPreview, state.showRecoveredPixels],
  );

  const composed: PixelBuffer | null = useMemo(() => {
    switch (state.compareMode) {
      case 'original':
        return original ? { ...original, data: original.pixels } : null;
      case 'converted':
        return converted ? { ...converted, data: converted.pixels } : null;
      case 'side-by-side':
        return composeSideBySide(original, converted);
      case 'reveal':
        return composeReveal(original, converted, state.reveal);
      default:
        return null;
    }
  }, [state.compareMode, state.reveal, original, converted]);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || composed === null) {
      return;
    }
    canvas.width = composed.width;
    canvas.height = composed.height;
    const context = canvas.getContext('2d');
    if (!context) {
      return;
    }
    context.putImageData(toImageData(composed), 0, 0);
  }, [composed]);

  const factor = zoomFactor(state.zoomMode);
  const hasPreview = composed !== null;

  return (
    <section className="panel" aria-label="Preview">
      <header className="panel-header">
        <div className="preview-toolbar" role="group" aria-label="Comparison mode">
          {COMPARE_MODES.map((mode) => (
            <button
              key={mode.id}
              type="button"
              className="preset-button"
              style={{ minWidth: 84 }}
              aria-pressed={state.compareMode === mode.id}
              title={mode.title}
              onClick={() => dispatch({ type: 'compare-mode-changed', compareMode: mode.id })}
            >
              {mode.label}
            </button>
          ))}
        </div>
        <div className="app-header-spacer" />
        <label className="checkbox-row">
          <input
            type="checkbox"
            checked={state.showRecoveredPixels}
            onChange={(event) =>
              dispatch({
                type: 'recovered-pixels-changed',
                showRecoveredPixels: event.target.checked,
              })
            }
          />
          Show recovered pixels
        </label>
        <div className="preview-toolbar" role="group" aria-label="Zoom">
          {ZOOM_MODES.map((mode) => (
            <button
              key={mode.id}
              type="button"
              className="preset-button"
              style={{ minWidth: 56 }}
              aria-pressed={state.zoomMode === mode.id}
              onClick={() => dispatch({ type: 'zoom-mode-changed', zoomMode: mode.id })}
            >
              {mode.label}
            </button>
          ))}
        </div>
      </header>

      <div className="preview-surface">
        {hasPreview ? (
          <canvas
            ref={canvasRef}
            className="preview-canvas"
            data-testid="preview-canvas"
            style={
              factor === null
                ? undefined
                : { width: Math.round(composed.width * factor), height: 'auto' }
            }
          />
        ) : (
          <p className="preview-placeholder">
            Choose a RAW file to see the original and the converted preview side by side.
            Everything is processed in this browser tab; nothing is uploaded.
          </p>
        )}
      </div>
    </section>
  );
}
