/**
 * The client for the WebAssembly module.
 *
 * The module runs in a worker so the page stays responsive while a RAW
 * file is decoded and a DNG is written.  This client owns that worker and
 * turns the message protocol of web/public/wasm/hiraco-worker.js into
 * promises.
 */

export interface WasmCapabilities {
  dngSdkEnabled: boolean;
  dngStatus: string;
}

/** The metadata of a prepared source, as reported by the bridge. */
export interface BridgeMetadata {
  make: string;
  model: string;
  unique_camera_model: string;
  flip: number;
  width: number;
  height: number;
  has_black_level: boolean;
  black_level: number;
  has_white_level: boolean;
  white_level: number;
  has_as_shot_neutral: boolean;
  as_shot_neutral: number[];
  has_exif_iso: boolean;
  exif_iso: number;
  has_exif_shutter_speed: boolean;
  exif_shutter_speed: number;
  has_exif_aperture: boolean;
  exif_aperture: number;
  has_working_geometry: boolean;
  working_width: number;
  working_height: number;
  has_default_crop: boolean;
  default_crop: number[];
  has_highlight_recovery: boolean;
}

export interface OpenedSource {
  sessionId: number;
  width: number;
  height: number;
  metadata: BridgeMetadata;
}

export interface BridgePreview {
  kind: 'original' | 'converted';
  width: number;
  height: number;
  /** RGBA, four bytes per pixel. */
  pixels: Uint8ClampedArray;
  /** One byte per pixel, 1 where the ORI highlight recovery changed it. */
  mask: Uint8Array;
}

export interface ProgressUpdate {
  phase: string;
  fraction: number;
  message: string;
}

interface PendingRequest {
  id: number;
  resolve: (value: unknown) => void;
  reject: (error: Error) => void;
}

/** Thrown when the conversion was cancelled by the user. */
export class CancelledError extends Error {
  constructor() {
    super('the conversion was cancelled');
    this.name = 'CancelledError';
  }
}

const WORKER_URL = 'wasm/hiraco-worker.js';

export class HiracoWasmClient {
  private worker: Worker | null = null;
  private pending: PendingRequest[] = [];
  private nextRequestId = 1;
  private ready: Promise<WasmCapabilities> | null = null;
  private progressListeners = new Set<(update: ProgressUpdate) => void>();
  private capabilities: WasmCapabilities | null = null;

  /** Reports the progress of the running operation. */
  onProgress(listener: (update: ProgressUpdate) => void): () => void {
    this.progressListeners.add(listener);
    return () => {
      this.progressListeners.delete(listener);
    };
  }

  /** Starts the worker and loads the module. */
  async init(): Promise<WasmCapabilities> {
    await this.ensureReady();
    return this.capabilities as WasmCapabilities;
  }

  /** Prepares a RAW file, with an optional ORI companion, for the session. */
  async open(raw: ArrayBuffer, rawName: string, companion?: ArrayBuffer, companionName?: string): Promise<OpenedSource> {
    const worker = await this.ensureReady();
    return (await this.request(worker, {
      type: 'open',
      raw,
      rawName,
      companion,
      companionName,
    })) as OpenedSource;
  }

  /** Renders the preview of the untouched source. */
  async originalPreview(maxDimension: number): Promise<BridgePreview> {
    const worker = await this.ensureReady();
    const reply = (await this.request(worker, {
      type: 'preview',
      kind: 'original',
      maxDimension,
    })) as Record<string, unknown>;
    return this.previewFromReply(reply, 'original');
  }

  /** Renders the preview of the processed source. */
  async convertedPreview(maxDimension: number, overrides: number[]): Promise<BridgePreview> {
    const worker = await this.ensureReady();
    const reply = (await this.request(worker, {
      type: 'preview',
      kind: 'converted',
      maxDimension,
      overrides,
    })) as Record<string, unknown>;
    return this.previewFromReply(reply, 'converted');
  }

  /** Reads the preview the worker rendered. */
  private previewFromReply(reply: Record<string, unknown>, kind: 'original' | 'converted'): BridgePreview {
    const pixels = reply.pixels;
    const mask = reply.mask;
    return {
      kind,
      width: Number(reply.width ?? 0),
      height: Number(reply.height ?? 0),
      pixels: new Uint8ClampedArray(pixels as ArrayBuffer),
      mask: mask ? new Uint8Array(mask as ArrayBuffer) : new Uint8Array(0),
    };
  }

  /** Asks the core for the settings it resolves for this image. */
  async resolvedSettings(overrides: number[]): Promise<number[]> {
    const worker = await this.ensureReady();
    const reply = (await this.request(worker, { type: 'settings', overrides })) as { values: number[] };
    return reply.values;
  }

  /** Asks the bridge for a one line summary of the writer configuration. */
  async writerSummary(compression: string): Promise<string> {
    const worker = await this.ensureReady();
    const reply = (await this.request(worker, { type: 'summary', compression })) as { text: string };
    return reply.text;
  }

  /** Writes the Linear DNG and returns its bytes. */
  async convert(compression: string, overrides: number[]): Promise<Uint8Array> {
    const worker = await this.ensureReady();
    const reply = (await this.request(worker, { type: 'convert', compression, overrides })) as {
      dng: ArrayBuffer;
      bytes: number;
    };
    return new Uint8Array(reply.dng);
  }

  /**
   * Stops the running conversion.
   *
   * The conversion runs inside the module, so the request cannot reach it
   * while it is busy.  The worker is therefore stopped, which ends the
   * conversion where it is; the next call starts a fresh worker.
   */
  cancel(): void {
    this.stopWorker(new CancelledError());
  }

  /** Releases the session and stops the worker. */
  dispose(): void {
    this.stopWorker(new Error('the session was closed'));
  }

  private ensureWorker(): Worker {
    if (this.worker) {
      return this.worker;
    }
    // The worker is a classic script: the module needs the script name of
    // the worker to start its own threads from it.
    const url = new URL(WORKER_URL, document.baseURI).href;
    const worker = new Worker(url);
    worker.onmessage = (event: MessageEvent) => this.handleMessage(event.data);
    worker.onerror = (event: ErrorEvent) => {
      this.stopWorker(new Error(event.message || 'the module failed to start'));
    };
    this.worker = worker;
    // The module is loaded once per worker.  A worker that was stopped
    // earlier, for example after a cancellation, starts over.
    this.ready = this.request(worker, { type: 'init' }).then((capabilities) => {
      this.capabilities = capabilities as WasmCapabilities;
      return this.capabilities;
    });
    return worker;
  }

  /** Returns the worker with a loaded module. */
  private async ensureReady(): Promise<Worker> {
    const worker = this.ensureWorker();
    await this.ready;
    return worker;
  }

  private request(worker: Worker, message: unknown): Promise<unknown> {
    const id = this.nextRequestId;
    this.nextRequestId += 1;
    return new Promise((resolve, reject) => {
      this.pending.push({ id, resolve, reject });
      worker.postMessage({ ...(message as Record<string, unknown>), id });
    });
  }

  private handleMessage(message: { type: string; id?: number } & Record<string, unknown>): void {
    switch (message.type) {
      case 'progress': {
        const update: ProgressUpdate = {
          phase: String(message.phase ?? ''),
          fraction: Number(message.fraction ?? 0),
          message: String(message.message ?? ''),
        };
        this.progressListeners.forEach((listener) => listener(update));
        return;
      }
      case 'log': {
        console.info(String(message.message ?? ''));
        return;
      }
      case 'error': {
        const stack = typeof message.stack === 'string' ? message.stack : '';
        if (stack) {
          console.error(stack);
        }
        const error = new Error(String(message.message ?? 'the module failed'));
        this.rejectAll(error);
        return;
      }
      default:
        break;
    }
    // Every reply carries the number of the request it answers, so a
    // message that belongs to another request is never mistaken for it.
    const index = this.pending.findIndex((request) => request.id === message.id);
    if (index < 0) {
      return;
    }
    const [request] = this.pending.splice(index, 1);
    request.resolve(message);
  }

  private rejectAll(error: Error): void {
    const pending = this.pending;
    this.pending = [];
    pending.forEach((request) => request.reject(error));
  }

  private stopWorker(error: Error): void {
    const worker = this.worker;
    this.worker = null;
    this.ready = null;
    this.capabilities = null;
    if (worker) {
      worker.terminate();
    }
    this.rejectAll(error);
  }
}
