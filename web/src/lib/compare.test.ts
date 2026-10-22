import { describe, expect, it } from 'vitest';
import {
  clampRevealFraction,
  previewDisplaySize,
  revealFractionFromPointer,
  zoomFactor,
} from './compare';

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

describe('reveal helpers', () => {
  it('clamps the reveal position', () => {
    expect(clampRevealFraction(-1)).toBe(0);
    expect(clampRevealFraction(2)).toBe(1);
    expect(clampRevealFraction(0.25)).toBe(0.25);
  });

  it('maps a pointer position to a reveal fraction', () => {
    expect(revealFractionFromPointer(150, { left: 100, width: 200 })).toBe(0.25);
    expect(revealFractionFromPointer(50, { left: 100, width: 200 })).toBe(0);
  });
});
