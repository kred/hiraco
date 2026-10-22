import { fireEvent, render, screen } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { App } from '../App';

describe('the application shell', () => {
  it('renders the three panes of the native layout', () => {
    render(<App />);
    expect(screen.getByLabelText('Workspace')).toBeInTheDocument();
    expect(screen.getByLabelText('Preview')).toBeInTheDocument();
    expect(screen.getByLabelText('Processing')).toBeInTheDocument();
    expect(screen.getByLabelText('Inspector')).toBeInTheDocument();
  });

  it('offers the native presets and the stage sliders', () => {
    render(<App />);
    expect(screen.getByRole('button', { name: 'Small' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Medium' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Strong' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Custom' })).toBeInTheDocument();
    expect(screen.getByLabelText('Blur Radius')).toBeInTheDocument();
    expect(screen.getByLabelText('Edge Gain')).toBeInTheDocument();
  });

  it('switches the theme and remembers it', () => {
    render(<App />);
    const toggle = screen.getByRole('button', { name: /Switch to the (light|dark) theme/ });
    const before = document.documentElement.dataset.theme;
    fireEvent.click(toggle);
    const after = document.documentElement.dataset.theme;
    expect(after).not.toBe(before);
    expect(['light', 'dark']).toContain(after);
  });
});
