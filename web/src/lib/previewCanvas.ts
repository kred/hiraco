import type { PreviewBuffer } from '../state/appState';

/** A plain RGBA pixel buffer, independent of the DOM. */
export interface PixelBuffer {
  width: number;
  height: number;
  data: Uint8ClampedArray;
}

/** Wrap the pixel buffer in an `ImageData` the canvas can draw. */
export function toImageData(buffer: PixelBuffer): ImageData {
  const data = new Uint8ClampedArray(buffer.data);
  return new ImageData(data, buffer.width, buffer.height);
}

/**
 * Put the original and the converted preview next to each other.
 */
export function composeSideBySide(
  original: PreviewBuffer | null,
  converted: PreviewBuffer | null,
): PixelBuffer | null {
  if (original === null && converted === null) {
    return null;
  }
  const left = original ?? converted!;
  const right = converted ?? original!;
  const width = left.width + right.width;
  const height = Math.max(left.height, right.height);
  const data = new Uint8ClampedArray(width * height * 4);

  const blit = (source: PreviewBuffer, offsetX: number) => {
    for (let row = 0; row < source.height; row += 1) {
      const target = (row * width + offsetX) * 4;
      const start = row * source.width * 4;
      data.set(source.pixels.subarray(start, start + source.width * 4), target);
    }
  };

  blit(left, 0);
  blit(right, left.width);
  return { width, height, data };
}

/**
 * Wipe between the original and the converted preview.  Everything left of
 * the split shows the original, everything right of it the result.
 */
export function composeReveal(
  original: PreviewBuffer | null,
  converted: PreviewBuffer | null,
  fraction: number,
): PixelBuffer | null {
  if (original === null && converted === null) {
    return null;
  }
  const width = Math.max(original?.width ?? 0, converted?.width ?? 0);
  const height = Math.max(original?.height ?? 0, converted?.height ?? 0);
  if (width <= 0 || height <= 0) {
    return null;
  }
  const split = Math.round(width * Math.min(1, Math.max(0, fraction)));
  const data = new Uint8ClampedArray(width * height * 4);

  for (let row = 0; row < height; row += 1) {
    for (let column = 0; column < width; column += 1) {
      const source = column < split ? original : converted;
      const target = (row * width + column) * 4;
      if (source === null || source === undefined) {
        continue;
      }
      if (row >= source.height || column >= source.width) {
        continue;
      }
      const start = (row * source.width + column) * 4;
      data[target] = source.pixels[start];
      data[target + 1] = source.pixels[start + 1];
      data[target + 2] = source.pixels[start + 2];
      data[target + 3] = source.pixels[start + 3];
    }
  }
  return { width, height, data };
}

/**
 * Mark the pixels the ORI highlight recovery restored, the way the native
 * "Show recovered pixels" checkbox does.
 */
export function applyRecoveredMask(
  preview: PreviewBuffer,
  showRecoveredPixels: boolean,
): PreviewBuffer {
  if (!showRecoveredPixels || preview.recoveredMask.length === 0) {
    return preview;
  }
  const pixels = new Uint8ClampedArray(preview.pixels);
  const count = Math.min(preview.recoveredMask.length, preview.width * preview.height);
  for (let index = 0; index < count; index += 1) {
    if (preview.recoveredMask[index] === 0) {
      continue;
    }
    const offset = index * 4;
    // The native application casts the recovered pixels to solid magenta.
    pixels[offset] = 255;
    pixels[offset + 1] = 0;
    pixels[offset + 2] = 255;
  }
  return { ...preview, pixels };
}
