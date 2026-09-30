// Local preview server for the built docs (public/).
//
// Why not `python -m http.server` / a generic static server:
//   1. Hugo fingerprints the JS/CSS assets but NOT the HTML. A rebuild after
//      content changes deletes the previously referenced hashed files, so a
//      browser holding a cached page asks for a file that no longer exists and
//      the page silently loses its scripts. Serving HTML with `no-store`
//      prevents that entire class of stale-preview bug.
//   2. Pagefind loads its wasm through WebAssembly.instantiateStreaming, which
//      requires `application/wasm`. Generic servers often guess octet-stream.
//
// Usage: node serve-local.mjs   (or: npm run preview)
// Serves ./public on PORT (default 1313) at http://127.0.0.1:<port>/

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(here, 'public');
const port = Number(process.env.PORT || 1313);
const host = process.env.HOST || '127.0.0.1';

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.txt': 'text/plain; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.map': 'application/json; charset=utf-8',
  '.xml': 'application/xml; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.gif': 'image/gif',
  '.webp': 'image/webp',
  '.avif': 'image/avif',
  '.ico': 'image/x-icon',
  '.woff': 'font/woff',
  '.woff2': 'font/woff2',
  '.ttf': 'font/ttf',
  '.wasm': 'application/wasm',
  // Pagefind: wasm.<lang>.pagefind is a wasm module, pf_index/pf_fragment are
  // binary index data fetched with fetch().
  '.pagefind': 'application/wasm',
  '.pf_index': 'application/octet-stream',
  '.pf_fragment': 'application/octet-stream',
  '.pf_meta': 'application/octet-stream',
};

// Only HTML is no-store: it is un-fingerprinted and must always match the
// hashed assets it references. Everything else keeps normal caching.
const NO_STORE = new Set(['.html', '.txt', '.json']);

const server = http.createServer((req, res) => {
  let pathname = '/';
  try {
    pathname = decodeURIComponent(new URL(req.url, `http://${req.headers.host || host}`).pathname);
  } catch {
    res.writeHead(400, { 'Content-Type': 'text/plain' });
    res.end('Bad request');
    return;
  }

  const target = path.join(root, pathname);
  if (!target.startsWith(root)) {
    res.writeHead(403, { 'Content-Type': 'text/plain' });
    res.end('Forbidden');
    return;
  }

  fs.stat(target, (err, st) => {
    if (!err && st.isDirectory()) {
      return send(path.join(target, 'index.html'), req, res);
    }
    if (err) return send(path.join(target, 'index.html'), req, res);
    return send(target, req, res);
  });
});

function send(file, req, res) {
  fs.stat(file, (err, st) => {
    if (err || !st.isFile()) {
      res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('Not found');
      return;
    }
    const ext = path.extname(file).toLowerCase();
    const headers = {
      'Content-Type': TYPES[ext] || 'application/octet-stream',
      'Content-Length': st.size,
      'Cache-Control': NO_STORE.has(ext) ? 'no-store' : 'no-cache',
    };
    if (req.method === 'HEAD') {
      res.writeHead(200, headers);
      res.end();
      return;
    }
    res.writeHead(200, headers);
    fs.createReadStream(file).on('error', () => res.destroy()).pipe(res);
  });
}

server.listen(port, host, () => {
  console.log(`Serving ${root} at http://${host}:${port}/`);
  console.log('HTML is sent with Cache-Control: no-store so previews never go stale.');
});
