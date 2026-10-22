import { useAppState } from '../state/AppStateProvider';

function formatOptional(value: number | undefined, suffix = ''): string {
  return value === undefined ? '—' : `${value}${suffix}`;
}

export function InspectorPanel() {
  const { state } = useAppState();
  const metadata = state.metadata;

  if (metadata === null) {
    return (
      <section className="panel" aria-label="Inspector">
        <header className="panel-header">
          <h2 className="panel-title">Inspector</h2>
        </header>
        <div className="panel-body tight">
          <p className="empty-state">No image loaded yet.</p>
        </div>
      </section>
    );
  }

  return (
    <section className="panel" aria-label="Inspector">
      <header className="panel-header">
        <h2 className="panel-title">Inspector</h2>
      </header>
      <div className="panel-body tight">
        <dl className="inspector-list">
          <dt>Make</dt>
          <dd>{metadata.make || '—'}</dd>
          <dt>Model</dt>
          <dd>{metadata.model || '—'}</dd>
          {metadata.uniqueCameraModel !== '' && (
            <>
              <dt>Camera</dt>
              <dd>{metadata.uniqueCameraModel}</dd>
            </>
          )}
          <dt>Working size</dt>
          <dd>
            {metadata.workingWidth} × {metadata.workingHeight}
          </dd>
          <dt>Black level</dt>
          <dd>{formatOptional(metadata.blackLevel)}</dd>
          <dt>White level</dt>
          <dd>{formatOptional(metadata.whiteLevel)}</dd>
          <dt>ISO</dt>
          <dd>{formatOptional(metadata.iso)}</dd>
          <dt>Shutter</dt>
          <dd>{metadata.shutterSpeed ?? '—'}</dd>
          <dt>Aperture</dt>
          <dd>{formatOptional(metadata.aperture)}</dd>
        </dl>
      </div>
    </section>
  );
}
