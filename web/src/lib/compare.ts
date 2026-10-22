/**
 * The preview comparison and zoom helpers.
 *
 * The native GUI offers three comparison modes (original, converted, side by
 * side) and three zoom modes (fit, 50 %, 100 %).  The browser adds a reveal
 * slider, which the design mockups show as a fourth way to compare.
 */

export type CompareMode = 'original' | 'converted' | 'side-by-side' | 'reveal';

export interface CompareModeSpec {
  id: CompareMode;
  label: string;
  title: string;
}

export const COMPARE_MODES: CompareModeSpec[] = [
  {
    id: 'original',
    label: 'Original',
    title: 'Show the decoded RAW file without any processing',
  },
  {
    id: 'converted',
    label: 'Converted',
    title: 'Show the processed result',
  },
  {
    id: 'side-by-side',
    label: 'Side by side',
    title: 'Show the original and the result next to each other',
  },
  {
    id: 'reveal',
    label: 'Reveal',
    title: 'Wipe between the original and the result',
  },
];

export type ZoomMode = 'fit' | '50' | '100';

export interface ZoomModeSpec {
  id: ZoomMode;
  label: string;
}

export const ZOOM_MODES: ZoomModeSpec[] = [
  { id: 'fit', label: 'Fit' },
  { id: '50', label: '50%' },
  { id: '100', label: '100%' },
];

/** The zoom factor a zoom mode asks for. */
export function zoomFactor(mode: ZoomMode): number | null {
  switch (mode) {
    case '50':
      return 0.5;
    case '100':
      return 1;
    case 'fit':
    default:
      return null;
  }
}

/**
 * The on-screen size of a preview in a zoom mode.  `fit` scales the image
 * down until it fits the available box, the other modes use the image's own
 * pixels.
 */
export function previewDisplaySize(
  mode: ZoomMode,
  imageWidth: number,
  imageHeight: number,
  availableWidth: number,
  availableHeight: number,
): { width: number; height: number } {
  if (imageWidth <= 0 || imageHeight <= 0) {
    return { width: 0, height: 0 };
  }
  const factor = zoomFactor(mode);
  if (factor !== null) {
    return {
      width: Math.round(imageWidth * factor),
      height: Math.round(imageHeight * factor),
    };
  }
  if (availableWidth <= 0 || availableHeight <= 0) {
    return { width: imageWidth, height: imageHeight };
  }
  const scale = Math.min(availableWidth / imageWidth, availableHeight / imageHeight);
  return {
    width: Math.max(1, Math.round(imageWidth * scale)),
    height: Math.max(1, Math.round(imageHeight * scale)),
  };
}

/** Keep the reveal position inside the image. */
export function clampRevealFraction(value: number): number {
  if (!Number.isFinite(value)) {
    return 0.5;
  }
  return value < 0 ? 0 : value > 1 ? 1 : value;
}

/** The reveal position for a pointer at `clientX` inside `rect`. */
export function revealFractionFromPointer(clientX: number, rect: {
  left: number;
  width: number;
}): number {
  if (rect.width <= 0) {
    return 0.5;
  }
  return clampRevealFraction((clientX - rect.left) / rect.width);
}
