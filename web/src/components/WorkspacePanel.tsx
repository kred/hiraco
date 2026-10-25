import { useRef } from 'react';
import { useAppState } from '../state/AppStateProvider';

export function WorkspacePanel() {
  const { state, actions } = useAppState();
  const sourceInput = useRef<HTMLInputElement>(null);
  const companionInput = useRef<HTMLInputElement>(null);

  return (
    <section className="panel" aria-label="Workspace">
      <header className="panel-header">
        <h2 className="panel-title">Workspace</h2>
      </header>
      <div className="panel-body">
        <div className="field">
          <span className="field-label">RAW file</span>
          <input
            ref={sourceInput}
            className="text-input"
            type="file"
            accept=".ORF,.ORI,.dng,.DNG,image/*"
            aria-label="RAW file"
            onChange={(event) => {
              const file = event.target.files?.[0];
              if (file) {
                actions.selectSource(file);
              }
            }}
          />
        </div>

        <div className="field">
          <span className="field-label">ORI companion (optional)</span>
          <input
            ref={companionInput}
            className="text-input"
            type="file"
            accept=".ORI,.ori"
            aria-label="ORI companion"
            onChange={(event) => {
              const file = event.target.files?.[0];
              actions.selectCompanion(file ?? null);
            }}
          />
          <p className="hint">
            Attaching the ORI companion turns on highlight recovery, exactly like the
            native application does when it finds the file next to the RAW file.
          </p>
        </div>

        {state.sourceName === null ? (
          <p className="empty-state">No file selected yet.</p>
        ) : (
          <dl className="inspector-list">
            <dt>File</dt>
            <dd>{state.sourceName}</dd>
            {state.companionName !== null && (
              <>
                <dt>Companion</dt>
                <dd>{state.companionName}</dd>
              </>
            )}
          </dl>
        )}
      </div>
    </section>
  );
}
