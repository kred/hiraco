import { StrictMode } from 'react';
import { fireEvent, render, screen } from '@testing-library/react';
import { beforeEach, describe, expect, it } from 'vitest';
import { App } from '../App';
import { DEFAULT_STAGE_SETTINGS } from '../lib/stageSettings';

function renderApp() {
  return render(
    <StrictMode>
      <App />
    </StrictMode>,
  );
}

describe('the application shell', () => {
  beforeEach(() => {
    localStorage.clear();
    sessionStorage.clear();
  });

  it('renders the three panes of the native layout', () => {
    renderApp();
    expect(screen.getByText('highres raw converter')).toBeInTheDocument();
    expect(screen.getByLabelText('Workspace')).toBeInTheDocument();
    expect(screen.getByLabelText('Preview')).toBeInTheDocument();
    expect(screen.getByLabelText('Processing')).toBeInTheDocument();
    expect(screen.getByLabelText('Inspector')).toBeInTheDocument();
  });

  it('offers the native presets and the stage sliders', () => {
    renderApp();
    expect(screen.getByRole('button', { name: 'Small' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Medium' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Strong' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Custom' })).toBeInTheDocument();
    expect(screen.getByLabelText('Blur Radius')).toBeInTheDocument();
    expect(screen.getByLabelText('Edge Gain')).toBeInTheDocument();
  });

  it('offers a multi-file queue and a single custom preset', () => {
    renderApp();
    expect(screen.getByLabelText('RAW file')).toHaveAttribute('multiple');
    expect(screen.getByRole('button', { name: 'Add files' })).toBeInTheDocument();
    expect(
      screen.getByRole('button', { name: 'Add companion ORI' }),
    ).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Clear all' })).toBeInTheDocument();
    expect(screen.getByRole('heading', { name: 'Queue' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Custom' })).toBeDisabled();
  });

  it('loads the saved custom preset on startup', () => {
    localStorage.setItem(
      'hiraco.custom-preset.v1',
      JSON.stringify({ ...DEFAULT_STAGE_SETTINGS, stage1_psf_sigma: 2.5 }),
    );
    renderApp();
    expect(screen.queryByText(/Custom slot is empty/)).not.toBeInTheDocument();
  });

  it('switches the theme', () => {
    renderApp();
    const toggle = screen.getByRole('button', { name: /Switch to the (light|dark) theme/ });
    const before = document.documentElement.dataset.theme;
    fireEvent.click(toggle);
    const after = document.documentElement.dataset.theme;
    expect(after).not.toBe(before);
    expect(['light', 'dark']).toContain(after);
  });
});
