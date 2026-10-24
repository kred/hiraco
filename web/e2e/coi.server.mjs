// Serves web/dist the way a static host serves it: as plain files, with no
// Cross-Origin-Opener-Policy and no Cross-Origin-Embedder-Policy headers.
// GitHub Pages behaves the same way, which is why the app relies on the
// service worker in public/coi-serviceworker.js there.  The isolation test
// runs against this server so it exercises that path instead of the header
// mechanism that vite preview provides.

import { readFile, stat } from 'node:fs/promises';
import { createServer } from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', 'dist');
const port = Number(process.env.PORT || 4174);

const contentTypes = {
  '.css': 'text/css; charset=utf-8',
  '.html': 'text/html; charset=utf-8',
  '.ico': 'image/x-icon',
  '.js': 'application/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.mjs': 'application/javascript; charset=utf-8',
  '.png': 'image/png',
  '.svg': 'image/svg+xml',
  '.wasm': 'application/wasm',
};

function isInsideRoot(filePath) {
  return filePath === root || filePath.startsWith(root + path.sep);
}

const server = createServer(async (request, response) => {
  try {
    const url = new URL(request.url ?? '/', `http://127.0.0.1:${port}`);
    let filePath = path.join(root, decodeURIComponent(url.pathname));
    if (!isInsideRoot(filePath)) {
      response.writeHead(403).end();
      return;
    }

    let info = await stat(filePath).catch(() => null);
    if (info?.isDirectory()) {
      filePath = path.join(filePath, 'index.html');
      info = await stat(filePath).catch(() => null);
    }
    if (!info?.isFile()) {
      response.writeHead(404).end('not found');
      return;
    }

    const body = await readFile(filePath);
    response.writeHead(200, {
      'Content-Type': contentTypes[path.extname(filePath)] ?? 'application/octet-stream',
      'Cache-Control': 'no-store',
    });
    response.end(body);
  } catch (error) {
    response.writeHead(500).end(String(error));
  }
});

server.listen(port, '127.0.0.1', () => {
  console.log(`serving ${root} on http://127.0.0.1:${port} without isolation headers`);
});
