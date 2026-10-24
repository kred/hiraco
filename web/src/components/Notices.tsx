import { useAppState } from '../state/AppStateProvider';

/**
 * The notices the application shows above the workspace: why the module
 * cannot run, and what went wrong in the last action.
 */
export function Notices() {
  const { state } = useAppState();

  if (state.moduleStatus === 'unsupported') {
    return (
      <div className="error-banner" role="alert" data-testid="module-unsupported">
        <strong>This browser cannot run the converter.</strong>
        <span>{state.moduleMessage}</span>
      </div>
    );
  }

  if (state.error !== null) {
    return (
      <div className="error-banner" role="alert" data-testid="error-banner">
        <span>{state.error}</span>
      </div>
    );
  }

  if (state.moduleStatus === 'loading') {
    return (
      <div className="notice" role="status" data-testid="module-loading">
        Loading the WebAssembly module…
      </div>
    );
  }

  return null;
}
