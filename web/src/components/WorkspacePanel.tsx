import { useRef } from 'react';
import { useAppState } from '../state/AppStateProvider';
import type { QueueItem, QueueStatus } from '../state/appState';
import { PRESET_LABELS } from '../lib/stageSettings';
import { IconButton } from './IconButton';
import {
  BookmarkIcon,
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
  const converting = state.converting;
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
          aria-hidden="true"
          tabIndex={-1}
          disabled={converting}
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
          aria-hidden="true"
          tabIndex={-1}
          disabled={converting}
          onChange={(event) => {
            const file = event.target.files?.[0] ?? null;
            event.target.value = '';
            actions.selectCompanion(file);
          }}
        />

        <div className="section queue-section">
          <div className="section-header queue-header">
            <h3 className="section-title" aria-label="Queue">
              Queue
              {state.queue.length > 0 && (
                <span className="queue-count" aria-hidden="true">
                  {state.queue.length}
                </span>
              )}
            </h3>
            <div className="queue-toolbar" role="group" aria-label="Queue actions">
              <IconButton
                label="Add files"
                description="Add one or more RAW or DNG files. ORI files pair automatically by file name."
                icon={<PlusIcon />}
                variant="primary"
                disabled={converting}
                onClick={() => sourceInput.current?.click()}
              />
              <IconButton
                label="Add companion ORI"
                description="Attach the ORI highlight-recovery companion of the selected file."
                icon={<LinkIcon />}
                disabled={converting || selectedItem === undefined}
                onClick={() => companionInput.current?.click()}
              />
              <span className="toolbar-separator" aria-hidden="true" />
              <IconButton
                label="Clear all"
                description="Remove every queued file and its settings."
                icon={<TrashIcon />}
                variant="danger"
                disabled={converting || state.queue.length === 0}
                onClick={() => actions.clearQueue()}
              />
            </div>
          </div>

          {state.queue.length === 0 ? (
            <p className="empty-state">
              No files queued yet.
              <span className="empty-hint">
                Add one or more RAW/DNG files; ORI companions pair by name.
              </span>
            </p>
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
                      data-testid="queue-select"
                      aria-current={selected ? 'true' : undefined}
                      title={queueDetails(item)}
                      disabled={converting}
                      onClick={() => actions.selectQueueItem(item.id)}
                    >
                      <span className="queue-name">
                        {selected && (
                          <span className="queue-selected-mark" aria-hidden="true">
                            ✓
                          </span>
                        )}
                        <span className="queue-name-text">{item.sourceName}</span>
                      </span>
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
                      disabled={converting}
                      onClick={() => actions.removeQueueItem(item.id)}
                    />
                  </li>
                );
              })}
            </ul>
          )}
        </div>
      </div>

      {selectedItem !== undefined && (
        <footer className="panel-footer queue-settings-footer">
          <div className="queue-settings" role="group" aria-label="Selected file settings">
            <IconButton
              label="Copy settings"
              description="Copy the resolved settings of the selected file."
              icon={<CopyIcon />}
              disabled={converting}
              onClick={() => actions.copySettings()}
            />
            <IconButton
              label="Paste settings"
              description="Paste the copied settings into the selected file."
              icon={<PasteIcon />}
              disabled={converting || !canPaste}
              onClick={() => actions.pasteSettings()}
            />
            <IconButton
              label="Paste to all files"
              description={`Paste the copied settings into all ${state.queue.length} queued files.`}
              icon={<LayersIcon />}
              disabled={converting || !canPaste}
              onClick={() => actions.pasteSettingsToAll()}
            />
            {selectedItem.companionName !== null && (
              <IconButton
                label={`Remove companion ${selectedItem.companionName}`}
                icon={<XIcon />}
                disabled={converting}
                onClick={() => actions.selectCompanion(null)}
              />
            )}
            <span className="toolbar-separator" aria-hidden="true" />
            <IconButton
              label="Set as custom preset"
              description="Save the selected file's resolved settings as the single Custom preset."
              icon={<BookmarkIcon />}
              disabled={converting}
              onClick={() => actions.setCustomPreset()}
            />
          </div>
          {state.customPreset === null && (
            <p className="hint">
              The bookmark saves this file's settings as the single Custom preset.
            </p>
          )}
        </footer>
      )}
    </section>
  );
}
