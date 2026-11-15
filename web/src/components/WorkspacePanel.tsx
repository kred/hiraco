import { useRef } from 'react';
import { useAppState } from '../state/AppStateProvider';
import type { QueueItem, QueueStatus } from '../state/appState';
import { PRESET_LABELS } from '../lib/stageSettings';

const STATUS_LABELS: Record<QueueStatus, string> = {
  pending: 'Queued',
  ready: 'Ready',
  working: 'Working',
  done: 'Converted',
  error: 'Failed',
  cancelled: 'Cancelled',
};

function QueueBadges({ item }: { item: QueueItem }) {
  return (
    <span className="queue-badges">
      <span className="queue-status" data-status={item.status}>
        {STATUS_LABELS[item.status]}
      </span>
      {item.width > 0 && item.height > 0 && (
        <span>
          {item.width}×{item.height}
        </span>
      )}
      {item.companionName !== null && <span className="queue-badge">ORI</span>}
      {item.preset !== null && (
        <span className="queue-badge">{PRESET_LABELS[item.preset]}</span>
      )}
    </span>
  );
}

export function WorkspacePanel() {
  const { state, actions } = useAppState();
  const sourceInput = useRef<HTMLInputElement>(null);
  const companionInput = useRef<HTMLInputElement>(null);
  const busy = state.activity.state === 'working';
  const selectedItem = state.queue.find((item) => item.id === state.selectedItemId);
  const canPaste = state.copiedSettings !== null;

  return (
    <section className="panel" aria-label="Workspace">
      <header className="panel-header">
        <h2 className="panel-title">Workspace</h2>
      </header>
      <div className="panel-body">
        <input
          ref={sourceInput}
          className="visually-hidden"
          type="file"
          multiple
          accept=".ORF,.ORI,.dng,.DNG,image/*"
          aria-label="RAW file"
          tabIndex={-1}
          onChange={(event) => {
            const files = Array.from(event.target.files ?? []);
            // Allow adding the same file again after removing it.
            event.target.value = '';
            actions.addFiles(files);
          }}
        />
        <input
          ref={companionInput}
          className="visually-hidden"
          type="file"
          accept=".ORI,.ori"
          aria-label="ORI companion"
          tabIndex={-1}
          onChange={(event) => {
            const file = event.target.files?.[0] ?? null;
            event.target.value = '';
            actions.selectCompanion(file);
          }}
        />

        <div className="section queue-section">
          <div className="section-header">
            <h3 className="section-title">
              Queue
              {state.queue.length > 0 && (
                <span className="queue-count">{state.queue.length}</span>
              )}
            </h3>
            <button
              type="button"
              className="button ghost"
              disabled={busy || state.queue.length === 0}
              onClick={() => actions.clearQueue()}
            >
              Clear all
            </button>
          </div>

          <div className="queue-toolbar">
            <button
              type="button"
              className="button primary"
              disabled={busy}
              onClick={() => sourceInput.current?.click()}
            >
              Add ORF
            </button>
            <button
              type="button"
              className="button"
              disabled={busy || selectedItem === undefined}
              title="Attach an ORI companion to the selected RAW file"
              onClick={() => companionInput.current?.click()}
            >
              Add companion ORI
            </button>
          </div>

          {state.queue.length === 0 ? (
            <p className="empty-state">No files queued yet.</p>
          ) : (
            <ul className="queue-list" aria-label="Conversion queue">
              {state.queue.map((item) => {
                const selected = item.id === state.selectedItemId;
                return (
                  <li
                    key={item.id}
                    className="queue-item"
                    data-testid="queue-item"
                    data-selected={selected ? 'true' : 'false'}
                    data-status={item.status}
                  >
                    <button
                      type="button"
                      className="queue-select"
                      aria-pressed={selected}
                      disabled={busy}
                      onClick={() => actions.selectQueueItem(item.id)}
                    >
                      <span className="queue-name">{item.sourceName}</span>
                      <QueueBadges item={item} />
                      {item.message !== null && item.status !== 'ready' && (
                        <span className="queue-message">{item.message}</span>
                      )}
                    </button>
                    <button
                      type="button"
                      className="queue-remove"
                      aria-label={`Remove ${item.sourceName}`}
                      title="Remove from the queue"
                      disabled={busy}
                      onClick={() => actions.removeQueueItem(item.id)}
                    >
                      ×
                    </button>
                  </li>
                );
              })}
            </ul>
          )}

          {selectedItem !== undefined && (
            <div className="queue-settings" aria-label="Queue settings">
              <span className="field-label">Selected file settings</span>
              <div className="button-row">
                <button
                  type="button"
                  className="button"
                  disabled={busy}
                  onClick={() => actions.copySettings()}
                >
                  Copy settings
                </button>
                <button
                  type="button"
                  className="button"
                  disabled={busy || !canPaste}
                  onClick={() => actions.pasteSettings()}
                >
                  Paste settings
                </button>
                <button
                  type="button"
                  className="button"
                  disabled={busy || !canPaste}
                  onClick={() => actions.pasteSettingsToAll()}
                >
                  Paste to all files
                </button>
              </div>
            </div>
          )}

          <p className="hint queue-hint">
            Presets and sliders edit the selected file. Copy settings, then paste
            them to another file or to all files in the queue.
          </p>
        </div>
      </div>
    </section>
  );
}
