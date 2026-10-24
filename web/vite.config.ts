import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

// The wasm module is produced by the CMake build (scripts/build_web.sh) and
// copied into public/wasm.  It is loaded at runtime, never bundled, so the
// app stays a plain static bundle that any static host can serve.
export default defineConfig({
  plugins: [react()],
  base: './',
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    target: 'es2022',
    assetsInlineLimit: 0,
  },
  server: {
    headers: {
      // The 64 bit wasm module uses a shared memory, which browsers only
      // expose to cross origin isolated documents.
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
  },
  preview: {
    headers: {
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
  },
  test: {
    environment: 'jsdom',
    globals: true,
    setupFiles: ['./src/test/setup.ts'],
    include: ['src/**/*.test.ts', 'src/**/*.test.tsx'],
  },
});
