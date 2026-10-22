import { ControlsPanel } from './components/ControlsPanel';
import { InspectorPanel } from './components/InspectorPanel';
import { PreviewPanel } from './components/PreviewPanel';
import { StatusBar } from './components/StatusBar';
import { WorkspacePanel } from './components/WorkspacePanel';
import { AppStateProvider } from './state/AppStateProvider';
import { ThemeProvider } from './theme/ThemeProvider';
import { ThemeToggle } from './theme/ThemeToggle';

export function App() {
  return (
    <ThemeProvider>
      <AppStateProvider>
        <div className="app">
          <header className="app-header">
            <div className="app-brand">
              hiraco <small>Linear DNG converter</small>
            </div>
            <div className="app-header-spacer" />
            <ThemeToggle />
          </header>
          <main className="app-main">
            <WorkspacePanel />
            <PreviewPanel />
            <div className="side-column">
              <ControlsPanel />
              <InspectorPanel />
            </div>
          </main>
          <StatusBar />
        </div>
      </AppStateProvider>
    </ThemeProvider>
  );
}
