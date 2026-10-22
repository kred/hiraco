import { ControlsPanel } from './components/ControlsPanel';
import { InspectorPanel } from './components/InspectorPanel';
import { Notices } from './components/Notices';
import { PreviewPanel } from './components/PreviewPanel';
import { StatusBar } from './components/StatusBar';
import { WorkspacePanel } from './components/WorkspacePanel';
import { AppStateProvider, useAppState } from './state/AppStateProvider';
import { ThemeProvider } from './theme/ThemeProvider';
import { ThemeToggle } from './theme/ThemeToggle';

function AppShell() {
  const { state } = useAppState();
  return (
    <div className="app" data-module-status={state.moduleStatus}>
      <header className="app-header">
        <div className="app-brand">
          hiraco <small>Linear DNG converter</small>
        </div>
        <div className="app-header-spacer" />
        <ThemeToggle />
      </header>
      <main className="app-main">
        <Notices />
        <WorkspacePanel />
        <PreviewPanel />
        <div className="side-column">
          <ControlsPanel />
          <InspectorPanel />
        </div>
      </main>
      <StatusBar />
    </div>
  );
}

export function App() {
  return (
    <ThemeProvider>
      <AppStateProvider>
        <AppShell />
      </AppStateProvider>
    </ThemeProvider>
  );
}
