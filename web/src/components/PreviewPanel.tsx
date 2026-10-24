import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { COMPARE_MODES, ZOOM_MODES, zoomFactor, previewDisplaySize } from '../lib/compare';
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
  const surfaceRef = useRef<HTMLDivElement>(null);
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

  // Measure the preview surface so "fit" can scale the image to it.
  const [available, setAvailable] = useState({ width: 0, height: 0 });
  useEffect(() => {
    const surface = surfaceRef.current;
    if (!surface || typeof ResizeObserver === 'undefined') {
      return;
    }
    const observer = new ResizeObserver((entries) => {
      for (const entry of entries) {
        const { width, height } = entry.contentRect;
        setAvailable({ width, height });
      }
    });
    observer.observe(surface);
    return () => observer.disconnect();
  }, []);

  // Pan offset, applied as a CSS translate3d.  Kept in a ref so dragging can
  // mutate it without re-rendering on every pointer move.
  const pan = useRef<{ x: number; y: number }>({ x: 0, y: 0 });
  const drag = useRef<{
    active: boolean;
    startX: number;
    startY: number;
    startPanX: number;
    startPanY: number;
    pointerId: number | null;
  }>({
    active: false,
    startX: 0,
    startY: 0,
    startPanX: 0,
    startPanY: 0,
    pointerId: null,
  });

  // The on-screen size of the composed buffer.  Zoom factors scale relative to
  // the real image, not the preview buffer: the buffer may be capped at
  // PREVIEW_MAX_DIMENSION, so a plain factor would silently display less than
  // the labelled zoom (50% of a capped 2560px buffer is 25% of an 80MP image).
  const displaySize = useMemo(() => {
    if (!composed) {
      return { width: 0, height: 0 };
    }
    if (factor !== null) {
      const ratio = state.imageWidth > 0 ? state.imageWidth / composed.width : 1;
      const scale = factor * ratio;
      return {
        width: Math.round(composed.width * scale),
        height: Math.round(composed.height * scale),
      };
    }
    return previewDisplaySize(
      'fit',
      composed.width,
      composed.height,
      available.width,
      available.height,
    );
  }, [composed, factor, state.imageWidth, available.width, available.height]);

  // Applies a pan offset, clamped so the image can never leave the visible
  // area.  The canvas is anchored at its center (translate(-50%, -50%)), so
  // the pan range is symmetric around zero.
  const applyPan = useCallback(
    (nextX: number, nextY: number) => {
      const maxPanX = Math.max(0, (displaySize.width - available.width) / 2);
      const maxPanY = Math.max(0, (displaySize.height - available.height) / 2);
      pan.current = {
        x: Math.min(Math.max(nextX, -maxPanX), maxPanX),
        y: Math.min(Math.max(nextY, -maxPanY), maxPanY),
      };
      const canvas = canvasRef.current;
      if (canvas) {
        canvas.style.transform = `translate(-50%, -50%) translate3d(${pan.current.x}px, ${pan.current.y}px, 0)`;
      }
    },
    [displaySize.width, displaySize.height, available.width, available.height],
  );

  // Re-clamp the pan and keep the canvas centered before every paint when
  // the image, zoom mode, surface size, or the element itself changes.
  useLayoutEffect(() => {
    applyPan(pan.current.x, pan.current.y);
  }, [applyPan, hasPreview]);

  const onPointerDown = useCallback(
    (event: React.PointerEvent<HTMLCanvasElement>) => {
      if (!composed || event.button !== 0) {
        return;
      }
      const canvas = canvasRef.current;
      if (!canvas) {
        return;
      }
      event.preventDefault();
      canvas.setPointerCapture(event.pointerId);
      drag.current = {
        active: true,
        startX: event.clientX,
        startY: event.clientY,
        startPanX: pan.current.x,
        startPanY: pan.current.y,
        pointerId: event.pointerId,
      };
      canvas.style.cursor = 'grabbing';
    },
    [composed],
  );

  const onPointerMove = useCallback(
    (event: React.PointerEvent<HTMLCanvasElement>) => {
      if (!drag.current.active || !composed) {
        return;
      }
      const dx = event.clientX - drag.current.startX;
      const dy = event.clientY - drag.current.startY;
      applyPan(drag.current.startPanX + dx, drag.current.startPanY + dy);
    },
    [composed, applyPan],
  );

  const endDrag = useCallback(() => {
    drag.current.active = false;
    const canvas = canvasRef.current;
    if (canvas && drag.current.pointerId !== null) {
      canvas.releasePointerCapture(drag.current.pointerId);
      drag.current.pointerId = null;
    }
    const currentCanvas = canvasRef.current;
    if (currentCanvas) {
      currentCanvas.style.cursor = 'grab';
    }
  }, []);

  const onPointerUp = useCallback(endDrag, [endDrag]);
  const onPointerCancel = useCallback(endDrag, [endDrag]);
  const onPointerLeave = useCallback(() => {
    if (!drag.current.active) {
      return;
    }
    endDrag();
  }, [drag.current.active, endDrag]);

  const onContextMenu = useCallback((event: React.MouseEvent) => {
    event.preventDefault();
  }, []);

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

      <div ref={surfaceRef} className="preview-surface">
        {hasPreview ? (
          <canvas
            ref={canvasRef}
            className="preview-canvas"
            data-testid="preview-canvas"
            style={{
              cursor: 'grab',
              touchAction: 'none',
              width: displaySize.width,
              height: displaySize.height,
            }}
            onPointerDown={onPointerDown}
            onPointerMove={onPointerMove}
            onPointerUp={onPointerUp}
            onPointerCancel={onPointerCancel}
            onPointerLeave={onPointerLeave}
            onContextMenu={onContextMenu}
          />
        ) : (
          <p className="preview-placeholder">
            Choose a RAW file to see the original and the converted preview side by side.
            Everything is processed in this browser tab; nothing is uploaded.
          </p>
        )}
      </div>

      {state.compareMode === 'reveal' && hasPreview && (
        <div className="slider-row reveal-row">
          <label htmlFor="reveal-slider">Reveal</label>
          <input
            id="reveal-slider"
            className="slider"
            type="range"
            min={0}
            max={1}
            step={0.01}
            value={state.reveal}
            onChange={(event) =>
              dispatch({ type: 'reveal-changed', reveal: Number(event.target.value) })
            }
          />
          <span className="slider-value">{Math.round(state.reveal * 100)}%</span>
        </div>
      )}
    </section>
  );
}
