import { describe, expect, it } from 'vitest';
import { previewDisplaySize, previewMaxDimension, zoomFactor } from './compare';

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

describe('preview buffer sizes', () => {
  const cap = 2560;

  it('asks for the uncapped preview at 100%', () => {
    expect(previewMaxDimension('100', 10386, cap)).toBe(0);
  });

  it('asks for half the real width at 50% for a large image', () => {
    // 50% displays the 10386 px image at 5193 px, so the buffer needs at
    // least that many pixels; the old cap would upscale it and blur.
    expect(previewMaxDimension('50', 10386, cap)).toBe(5193);
  });

  it('keeps the cap at 50% for an image smaller than the cap', () => {
    expect(previewMaxDimension('50', 2000, cap)).toBe(cap);
  });

  it('keeps the cap for the fit view', () => {
    expect(previewMaxDimension('fit', 10386, cap)).toBe(cap);
  });
});
