/**
 * The preview comparison and zoom helpers.
 *
 * The browser offers the same three comparison modes as the native GUI
 * (original, converted, side by side) and three zoom modes (fit, 50 %, 100 %).
 */

export type CompareMode = 'original' | 'converted' | 'side-by-side';

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
 * The preview buffer dimension a zoom mode needs so the preview is never
 * upscaled on screen.
 *
 * 100% shows real image pixels one to one, so it asks for the uncapped
 * preview (0 means no limit).  At 50% the image is displayed at half its real
 * width, so half of the source resolution is enough; fetching the capped
 * preview there would be upscaled and look blurry.  The fit view always
 * downscales whatever it gets, so the cap is fine.
 */
export function previewMaxDimension(
  mode: ZoomMode,
  imageWidth: number,
  cap: number,
): number {
  if (mode === '100') {
    return 0;
  }
  if (mode === '50') {
    return Math.max(cap, Math.ceil(imageWidth / 2));
  }
  return cap;
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
