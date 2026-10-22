import {
  createContext,
  useContext,
  useMemo,
  useReducer,
  type Dispatch,
  type ReactNode,
} from 'react';
import {
  initialAppState,
  type AppAction,
  type AppState,
} from './appState';

function reducer(state: AppState, action: AppAction): AppState {
  switch (action.type) {
    case 'source-selected':
      return {
        ...state,
        sourceName: action.name,
        originalPreview: null,
        convertedPreview: null,
        metadata: null,
        result: null,
        resultName: null,
        error: null,
        activity: { state: 'idle', message: `Loaded ${action.name}`, fraction: null },
      };
    case 'companion-selected':
      return { ...state, companionName: action.name };
    case 'source-prepared':
      return {
        ...state,
        imageWidth: action.width,
        imageHeight: action.height,
        metadata: action.metadata,
        baseSettings: action.baseSettings,
        settings: action.baseSettings,
        overrides: {},
        preset: null,
      };
    case 'original-preview':
      return { ...state, originalPreview: action.preview };
    case 'converted-preview':
      return { ...state, convertedPreview: action.preview };
    case 'settings-changed':
      return {
        ...state,
        settings: action.settings,
        overrides: action.overrides,
        preset: null,
      };
    case 'preset-applied':
      return {
        ...state,
        settings: action.settings,
        overrides: action.overrides,
        preset: action.preset,
      };
    case 'base-settings-changed':
      return { ...state, baseSettings: action.baseSettings };
    case 'compression-changed':
      return { ...state, compression: action.compression };
    case 'compare-mode-changed':
      return { ...state, compareMode: action.compareMode };
    case 'zoom-mode-changed':
      return { ...state, zoomMode: action.zoomMode };
    case 'reveal-changed':
      return { ...state, reveal: action.reveal };
    case 'recovered-pixels-changed':
      return { ...state, showRecoveredPixels: action.showRecoveredPixels };
    case 'activity':
      return { ...state, activity: action.activity };
    case 'error':
      return { ...state, error: action.message };
    case 'result':
      return {
        ...state,
        result: action.blob,
        resultName: action.name,
        activity: { state: 'done', message: `Wrote ${action.name}`, fraction: 1 },
      };
    case 'reset':
      return initialAppState;
    default:
      return state;
  }
}

interface AppStateContextValue {
  state: AppState;
  dispatch: Dispatch<AppAction>;
}

const AppStateContext = createContext<AppStateContextValue | null>(null);

export function AppStateProvider({ children }: { children: ReactNode }) {
  const [state, dispatch] = useReducer(reducer, initialAppState);
  const value = useMemo(() => ({ state, dispatch }), [state]);
  return <AppStateContext.Provider value={value}>{children}</AppStateContext.Provider>;
}

export function useAppState(): AppStateContextValue {
  const value = useContext(AppStateContext);
  if (value === null) {
    throw new Error('useAppState must be used inside an AppStateProvider');
  }
  return value;
}
