import type { PreviewBuffer } from '../state/appState';

/** A plain RGBA pixel buffer, independent of the DOM. */
export interface PixelBuffer {
  width: number;
  height: number;
  data: Uint8ClampedArray;
}

/** Wrap the pixel buffer in an `ImageData` the canvas can draw. */
export function toImageData(buffer: PixelBuffer): ImageData {
  // ImageData keeps the array it is given by reference, so wrapping avoids a
  // full copy of the buffer - hundreds of megabytes for a full resolution
  // preview.  The buffer is never mutated after it is displayed.  The pixel
  // data always comes from a transferred ArrayBuffer, never from a shared one,
  // which the cast records for TypeScript.
  return new ImageData(
    buffer.data as Uint8ClampedArray<ArrayBuffer>,
    buffer.width,
    buffer.height,
  );
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
