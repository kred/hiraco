import { useRef } from 'react';
import { useAppState } from '../state/AppStateProvider';
import type { QueueItem, QueueStatus } from '../state/appState';
import { PRESET_LABELS } from '../lib/stageSettings';
import { IconButton } from './IconButton';
import {
  CopyIcon,
  LayersIcon,
  LinkIcon,
  PasteIcon,
  PlusIcon,
  TrashIcon,
  XIcon,
} from './icons';

const STATUS_LABELS: Record<QueueStatus, string> = {
  pending: 'Queued',
  ready: 'Ready',
  working: 'Working',
  done: 'Converted',
  error: 'Failed',
  cancelled: 'Cancelled',
};

/** Tooltip text: the compact rows only show the name and status. */
function queueDetails(item: QueueItem): string {
  const parts = [item.sourceName, STATUS_LABELS[item.status]];
  if (item.width > 0 && item.height > 0) {
    parts.push(`${item.width}×${item.height}`);
  }
  if (item.preset !== null) {
    parts.push(`${PRESET_LABELS[item.preset]} preset`);
  }
  if (item.companionName !== null) {
    parts.push(`ORI companion: ${item.companionName}`);
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
          <div className="section-header queue-header">
            <h3 className="section-title">
              Queue
              {state.queue.length > 0 && (
                <span className="queue-count">{state.queue.length}</span>
              )}
            </h3>
            <div className="queue-toolbar" role="group" aria-label="Queue actions">
              <IconButton
                label="Add ORF"
                icon={<PlusIcon />}
                variant="primary"
                disabled={busy}
                onClick={() => sourceInput.current?.click()}
              />
              <IconButton
                label="Add companion ORI"
                icon={<LinkIcon />}
                disabled={busy || selectedItem === undefined}
                onClick={() => companionInput.current?.click()}
              />
              <span className="toolbar-separator" aria-hidden="true" />
              <IconButton
                label="Clear all"
                icon={<TrashIcon />}
                variant="danger"
                disabled={busy || state.queue.length === 0}
                onClick={() => actions.clearQueue()}
              />
            </div>
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
                      title={queueDetails(item)}
                      disabled={busy}
                      onClick={() => actions.selectQueueItem(item.id)}
                    >
                      <span className="queue-name">{item.sourceName}</span>
                      <span className="queue-meta">
                        <span className="queue-dot" aria-hidden="true" />
                        {STATUS_LABELS[item.status]}
                        {item.companionName !== null && ' · ORI'}
                        {item.preset !== null && ` · ${PRESET_LABELS[item.preset]}`}
                      </span>
                      {item.message !== null && item.status !== 'ready' && (
                        <span className="queue-message">{item.message}</span>
                      )}
                    </button>
                    <IconButton
                      label={`Remove ${item.sourceName}`}
                      icon={<XIcon />}
                      className="queue-remove"
                      disabled={busy}
                      onClick={() => actions.removeQueueItem(item.id)}
                    />
                  </li>
                );
              })}
            </ul>
          )}

          {selectedItem !== undefined && (
            <div className="queue-settings" role="group" aria-label="Selected file settings">
              <IconButton
                label="Copy settings"
                icon={<CopyIcon />}
                disabled={busy}
                onClick={() => actions.copySettings()}
              />
              <IconButton
                label="Paste settings"
                icon={<PasteIcon />}
                disabled={busy || !canPaste}
                onClick={() => actions.pasteSettings()}
              />
              <IconButton
                label="Paste to all files"
                icon={<LayersIcon />}
                disabled={busy || !canPaste}
                onClick={() => actions.pasteSettingsToAll()}
              />
            </div>
          )}
        </div>
      </div>
    </section>
  );
}
