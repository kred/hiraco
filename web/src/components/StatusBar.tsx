import { useAppState } from '../state/AppStateProvider';

export function StatusBar() {
  const { state } = useAppState();
  const { activity } = state;
  const percent = activity.fraction === null ? null : Math.round(activity.fraction * 100);

  return (
    <footer className="app-status status-bar" aria-label="Status">
      <span
        className={`status-dot ${
          activity.state === 'working'
            ? 'busy'
            : activity.state === 'error'
              ? 'error'
              : activity.state === 'done'
                ? 'ok'
                : ''
        }`}
        aria-hidden="true"
      />
      <span role="status">{activity.message}</span>
      {percent !== null && (
        <>
          <div
            className="progress"
            role="progressbar"
            aria-valuemin={0}
            aria-valuemax={100}
            aria-valuenow={percent}
          >
            <div className="progress-fill" style={{ width: `${percent}%` }} />
          </div>
          <span className="slider-value">{percent}%</span>
        </>
      )}
    </footer>
  );
}
