import { useRef } from 'react';
import { useAppState } from '../state/AppStateProvider';
import type { QueueItem, QueueStatus } from '../state/appState';

const STATUS_LABELS: Record<QueueStatus, string> = {
  pending: 'Queued',
  ready: 'Ready',
  working: 'Working',
  done: 'Converted',
  error: 'Failed',
  cancelled: 'Cancelled',
};

function queueDetail(item: QueueItem): string {
  const parts = [STATUS_LABELS[item.status]];
  if (item.width > 0 && item.height > 0) {
    parts.push(`${item.width}×${item.height}`);
  }
  if (item.companionName !== null) {
    parts.push('ORI');
  }
  if (item.message !== null && item.status !== 'ready') {
    parts.push(item.message);
  }
  return parts.join(' · ');
}

export function WorkspacePanel() {
  const { state, actions } = useAppState();
  const sourceInput = useRef<HTMLInputElement>(null);
  const companionInput = useRef<HTMLInputElement>(null);
  const busy = state.activity.state === 'working';
  const selectedItem = state.queue.find((item) => item.id === state.selectedItemId);

  return (
    <section className="panel" aria-label="Workspace">
      <header className="panel-header">
        <h2 className="panel-title">Workspace</h2>
      </header>
      <div className="panel-body">
        <div className="field">
          <span className="field-label">RAW files</span>
          <input
            ref={sourceInput}
            className="text-input"
            type="file"
            multiple
            accept=".ORF,.ORI,.dng,.DNG,image/*"
            aria-label="RAW file"
            disabled={busy}
            onChange={(event) => {
              const files = Array.from(event.target.files ?? []);
              // Allow adding the same file again after removing it.
              event.target.value = '';
              actions.addFiles(files);
            }}
          />
          <p className="hint">
            Add several files at once. ORI files with the same name are paired as the
            companion of their RAW automatically.
          </p>
        </div>

        <div className="field">
          <span className="field-label">ORI companion (selected file)</span>
          <input
            ref={companionInput}
            className="text-input"
            type="file"
            accept=".ORI,.ori"
            aria-label="ORI companion"
            disabled={busy || selectedItem === undefined}
            onChange={(event) => {
              const file = event.target.files?.[0] ?? null;
              event.target.value = '';
              actions.selectCompanion(file);
            }}
          />
          <p className="hint">
            Attaching the ORI companion turns on highlight recovery, exactly like the
            native application does when it finds the file next to the RAW file.
          </p>
        </div>

        <div className="section queue-section">
          <div className="section-header">
            <h3 className="section-title">Queue</h3>
            <button
              type="button"
              className="button ghost"
              disabled={busy || state.queue.length === 0}
              onClick={() => actions.clearQueue()}
            >
              Clear all
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
                      <span className="queue-meta">{queueDetail(item)}</span>
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

          <p className="hint queue-hint">
            Presets and sliders apply to every queued file. Convert writes one DNG per
            file.
          </p>
        </div>
      </div>
    </section>
  );
}
