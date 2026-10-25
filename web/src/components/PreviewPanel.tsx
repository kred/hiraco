import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { COMPARE_MODES, ZOOM_MODES, zoomFactor, previewDisplaySize } from '../lib/compare';
import { applyRecoveredMask, toImageData, type PixelBuffer } from '../lib/previewCanvas';
import { useAppState } from '../state/AppStateProvider';
import type { PreviewBuffer } from '../state/appState';

type PaneId = 'left' | 'right';

export function PreviewPanel() {
  const { state, dispatch } = useAppState();
  const surfaceRef = useRef<HTMLDivElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const leftCanvasRef = useRef<HTMLCanvasElement>(null);
  const rightCanvasRef = useRef<HTMLCanvasElement>(null);

  const original = state.originalPreview;
  const converted = useMemo(
    () =>
      state.convertedPreview
        ? applyRecoveredMask(state.convertedPreview, state.showRecoveredPixels)
        : null,
    [state.convertedPreview, state.showRecoveredPixels],
  );

  const sideBySide = state.compareMode === 'side-by-side';

  // The single-canvas modes (original and converted).
  const single: PixelBuffer | null = useMemo(() => {
    if (sideBySide) {
      return null;
    }
    switch (state.compareMode) {
      case 'original':
        return original ? { ...original, data: original.pixels } : null;
      case 'converted':
        return converted ? { ...converted, data: converted.pixels } : null;
      default:
        return null;
    }
  }, [sideBySide, state.compareMode, original, converted]);

  // Side by side: two fixed panes, each showing its own preview.  While one
  // preview is still missing both panes show the available one.
  const leftBuffer = sideBySide ? (original ?? converted) : null;
  const rightBuffer = sideBySide ? (converted ?? original) : null;

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || single === null) {
      return;
    }
    canvas.width = single.width;
    canvas.height = single.height;
    const context = canvas.getContext('2d');
    if (!context) {
      return;
    }
    context.putImageData(toImageData(single), 0, 0);
  }, [single]);

  const drawPane = useCallback(
    (canvas: HTMLCanvasElement | null, buffer: PreviewBuffer | null) => {
      if (!canvas || buffer === null) {
        return;
      }
      canvas.width = buffer.width;
      canvas.height = buffer.height;
      const context = canvas.getContext('2d');
      if (!context) {
        return;
      }
      context.putImageData(
        toImageData({ width: buffer.width, height: buffer.height, data: buffer.pixels }),
        0,
        0,
      );
    },
    [],
  );

  useEffect(() => {
    drawPane(leftCanvasRef.current, leftBuffer);
  }, [drawPane, leftBuffer]);

  useEffect(() => {
    drawPane(rightCanvasRef.current, rightBuffer);
  }, [drawPane, rightBuffer]);

  const factor = zoomFactor(state.zoomMode);
  const hasPreview = sideBySide ? leftBuffer !== null || rightBuffer !== null : single !== null;

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

  // ---------------------------------------------------------------------
  // Single-canvas modes: one pan offset, applied as a CSS translate3d.
  // ---------------------------------------------------------------------

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

  // The on-screen size of the single buffer.  Zoom factors scale relative to
  // the real image, not the preview buffer: the buffer may be capped at
  // PREVIEW_MAX_DIMENSION, so a plain factor would silently display less than
  // the labelled zoom (50% of a capped 2560px buffer is 25% of an 80MP image).
  const displaySize = useMemo(() => {
    if (!single) {
      return { width: 0, height: 0 };
    }
    if (factor !== null) {
      const ratio = state.imageWidth > 0 ? state.imageWidth / single.width : 1;
      const scale = factor * ratio;
      return {
        width: Math.round(single.width * scale),
        height: Math.round(single.height * scale),
      };
    }
    return previewDisplaySize(
      'fit',
      single.width,
      single.height,
      available.width,
      available.height,
    );
  }, [single, factor, state.imageWidth, available.width, available.height]);

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
      if (!single || event.button !== 0) {
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
    [single],
  );

  const onPointerMove = useCallback(
    (event: React.PointerEvent<HTMLCanvasElement>) => {
      if (!drag.current.active || !single) {
        return;
      }
      const dx = event.clientX - drag.current.startX;
      const dy = event.clientY - drag.current.startY;
      applyPan(drag.current.startPanX + dx, drag.current.startPanY + dy);
    },
    [single, applyPan],
  );

  const endDrag = useCallback(() => {
    drag.current.active = false;
    const canvas = canvasRef.current;
    if (canvas && drag.current.pointerId !== null) {
      canvas.releasePointerCapture(drag.current.pointerId);
      drag.current.pointerId = null;
    }
    if (canvas) {
      canvas.style.cursor = 'grab';
    }
  }, []);

  const onPointerUp = useCallback(endDrag, [endDrag]);
  const onPointerCancel = useCallback(endDrag, [endDrag]);
  const onPointerLeave = useCallback(() => {
    if (!drag.current.active) {
      return;
    }
    endDrag();
  }, [endDrag]);

  // ---------------------------------------------------------------------
  // Side by side: two fixed panes with one shared view.
  //
  // The pan position is a real-image coordinate, so dragging either pane
  // shows the same source region in both, like the native GUI.  The pane
  // frames themselves never move.
  // ---------------------------------------------------------------------

  const paneGap = 2;
  const paneWidth = Math.max(0, Math.floor((available.width - paneGap) / 2));
  const paneHeight = available.height;

  // Fixed zoom factors refer to the real image, not to the possibly capped
  // preview buffer: at 50 % the buffer may hold only 2560 px of an 8000 px
  // image, so a plain factor would display the wrong magnification.  This is
  // the same correction the single-canvas path applies.
  const paneDisplaySize = useCallback(
    (buffer: PreviewBuffer | null) => {
      if (!buffer) {
        return { width: 0, height: 0 };
      }
      if (factor !== null) {
        const ratio = state.imageWidth > 0 ? state.imageWidth / buffer.width : 1;
        const scale = factor * ratio;
        return {
          width: Math.round(buffer.width * scale),
          height: Math.round(buffer.height * scale),
        };
      }
      return previewDisplaySize('fit', buffer.width, buffer.height, paneWidth, paneHeight);
    },
    [factor, state.imageWidth, paneWidth, paneHeight],
  );

  const leftSize = useMemo(() => paneDisplaySize(leftBuffer), [paneDisplaySize, leftBuffer]);
  const rightSize = useMemo(() => paneDisplaySize(rightBuffer), [paneDisplaySize, rightBuffer]);

  const imageWidth =
    state.imageWidth > 0 ? state.imageWidth : (leftBuffer?.width ?? rightBuffer?.width ?? 0);
  const imageHeight =
    state.imageHeight > 0 ? state.imageHeight : (leftBuffer?.height ?? rightBuffer?.height ?? 0);

  // The pan range is the tightest one the two panes allow, measured in real
  // image pixels.
  const viewLimits = useMemo(() => {
    const limitsX: number[] = [];
    const limitsY: number[] = [];
    if (imageWidth > 0 && imageHeight > 0) {
      if (leftSize.width > 0 && leftSize.height > 0) {
        limitsX.push(
          (Math.max(0, (leftSize.width - paneWidth) / 2) * imageWidth) / leftSize.width,
        );
        limitsY.push(
          (Math.max(0, (leftSize.height - paneHeight) / 2) * imageHeight) / leftSize.height,
        );
      }
      if (rightSize.width > 0 && rightSize.height > 0) {
        limitsX.push(
          (Math.max(0, (rightSize.width - paneWidth) / 2) * imageWidth) / rightSize.width,
        );
        limitsY.push(
          (Math.max(0, (rightSize.height - paneHeight) / 2) * imageHeight) / rightSize.height,
        );
      }
    }
    return {
      x: limitsX.length > 0 ? Math.min(...limitsX) : 0,
      y: limitsY.length > 0 ? Math.min(...limitsY) : 0,
    };
  }, [
    leftSize.width,
    leftSize.height,
    rightSize.width,
    rightSize.height,
    paneWidth,
    paneHeight,
    imageWidth,
    imageHeight,
  ]);

  const view = useRef<{ x: number; y: number }>({ x: 0, y: 0 });
  const paneDrag = useRef<{
    active: boolean;
    pane: PaneId | null;
    pointerId: number | null;
    startX: number;
    startY: number;
    startViewX: number;
    startViewY: number;
  }>({
    active: false,
    pane: null,
    pointerId: null,
    startX: 0,
    startY: 0,
    startViewX: 0,
    startViewY: 0,
  });

  const applyView = useCallback(
    (nextX: number, nextY: number) => {
      view.current = {
        x: Math.min(Math.max(nextX, -viewLimits.x), viewLimits.x),
        y: Math.min(Math.max(nextY, -viewLimits.y), viewLimits.y),
      };
      const movePane = (
        canvas: HTMLCanvasElement | null,
        size: { width: number; height: number },
      ) => {
        if (
          !canvas ||
          size.width <= 0 ||
          size.height <= 0 ||
          imageWidth <= 0 ||
          imageHeight <= 0
        ) {
          return;
        }
        const x = (view.current.x * size.width) / imageWidth;
        const y = (view.current.y * size.height) / imageHeight;
        canvas.style.transform = `translate(-50%, -50%) translate3d(${x}px, ${y}px, 0)`;
      };
      movePane(leftCanvasRef.current, leftSize);
      movePane(rightCanvasRef.current, rightSize);
    },
    [viewLimits.x, viewLimits.y, leftSize, rightSize, imageWidth, imageHeight],
  );

  useLayoutEffect(() => {
    if (sideBySide) {
      applyView(view.current.x, view.current.y);
    }
  }, [applyView, sideBySide, leftBuffer, rightBuffer]);

  const onPanePointerDown = useCallback(
    (pane: PaneId, event: React.PointerEvent<HTMLCanvasElement>) => {
      if (!hasPreview || event.button !== 0) {
        return;
      }
      event.preventDefault();
      event.currentTarget.setPointerCapture(event.pointerId);
      paneDrag.current = {
        active: true,
        pane,
        pointerId: event.pointerId,
        startX: event.clientX,
        startY: event.clientY,
        startViewX: view.current.x,
        startViewY: view.current.y,
      };
      event.currentTarget.style.cursor = 'grabbing';
    },
    [hasPreview],
  );

  const onPanePointerMove = useCallback(
    (event: React.PointerEvent<HTMLCanvasElement>) => {
      const current = paneDrag.current;
      if (!current.active || current.pane === null) {
        return;
      }
      const size = current.pane === 'left' ? leftSize : rightSize;
      if (size.width <= 0 || size.height <= 0 || imageWidth <= 0 || imageHeight <= 0) {
        return;
      }
      const dx = ((event.clientX - current.startX) * imageWidth) / size.width;
      const dy = ((event.clientY - current.startY) * imageHeight) / size.height;
      applyView(current.startViewX + dx, current.startViewY + dy);
    },
    [leftSize, rightSize, imageWidth, imageHeight, applyView],
  );

  const endPaneDrag = useCallback((event: React.PointerEvent<HTMLCanvasElement>) => {
    const current = paneDrag.current;
    current.active = false;
    current.pane = null;
    const canvas = event.currentTarget;
    if (current.pointerId !== null && canvas.hasPointerCapture(current.pointerId)) {
      canvas.releasePointerCapture(current.pointerId);
    }
    current.pointerId = null;
    canvas.style.cursor = 'grab';
  }, []);

  const onPanePointerUp = useCallback(
    (event: React.PointerEvent<HTMLCanvasElement>) => endPaneDrag(event),
    [endPaneDrag],
  );
  const onPanePointerCancel = useCallback(
    (event: React.PointerEvent<HTMLCanvasElement>) => endPaneDrag(event),
    [endPaneDrag],
  );
  const onPanePointerLeave = useCallback(
    (event: React.PointerEvent<HTMLCanvasElement>) => {
      if (!paneDrag.current.active) {
        return;
      }
      endPaneDrag(event);
    },
    [endPaneDrag],
  );

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
        {sideBySide && hasPreview ? (
          <div className="preview-split" style={{ gap: paneGap }}>
            <div className="preview-pane">
              <canvas
                ref={leftCanvasRef}
                className="preview-canvas"
                data-testid="preview-canvas-left"
                style={{
                  cursor: 'grab',
                  touchAction: 'none',
                  width: leftSize.width,
                  height: leftSize.height,
                }}
                onPointerDown={(event) => onPanePointerDown('left', event)}
                onPointerMove={onPanePointerMove}
                onPointerUp={onPanePointerUp}
                onPointerCancel={onPanePointerCancel}
                onPointerLeave={onPanePointerLeave}
                onContextMenu={onContextMenu}
              />
            </div>
            <div className="preview-pane">
              <canvas
                ref={rightCanvasRef}
                className="preview-canvas"
                data-testid="preview-canvas-right"
                style={{
                  cursor: 'grab',
                  touchAction: 'none',
                  width: rightSize.width,
                  height: rightSize.height,
                }}
                onPointerDown={(event) => onPanePointerDown('right', event)}
                onPointerMove={onPanePointerMove}
                onPointerUp={onPanePointerUp}
                onPointerCancel={onPanePointerCancel}
                onPointerLeave={onPanePointerLeave}
                onContextMenu={onContextMenu}
              />
            </div>
          </div>
        ) : single ? (
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
    </section>
  );
}
