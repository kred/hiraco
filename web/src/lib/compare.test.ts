import { describe, expect, it } from 'vitest';
import { previewDisplaySize, zoomFactor } from './compare';

describe('zoom helpers', () => {
  it('knows the fixed zoom factors', () => {
    expect(zoomFactor('50')).toBe(0.5);
    expect(zoomFactor('100')).toBe(1);
    expect(zoomFactor('fit')).toBeNull();
  });

  it('scales the image for the fixed zoom modes', () => {
    expect(previewDisplaySize('50', 1000, 500, 800, 600)).toEqual({
      width: 500,
      height: 250,
    });
  });

  it('shrinks the image to fit the available box', () => {
    expect(previewDisplaySize('fit', 1000, 500, 400, 400)).toEqual({
      width: 400,
      height: 200,
    });
  });

  it('keeps the aspect ratio when fitting', () => {
    const size = previewDisplaySize('fit', 1000, 1000, 400, 200);
    expect(size.width).toBe(200);
    expect(size.height).toBe(200);
  });
});
