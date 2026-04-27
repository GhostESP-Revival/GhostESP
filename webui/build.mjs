import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';

const root = path.dirname(new URL(import.meta.url).pathname).replace(/^\/(.:)/, '$1');
const sourceDir = path.join(root, 'src');
const indexPath = path.join(sourceDir, 'index.html');
const outputDir = path.join(root, 'dist');
const outputPath = path.join(outputDir, 'ghost_site.html');

function readSource(fileName) {
  return fs.readFileSync(path.join(sourceDir, fileName), 'utf8');
}

function stripCssComments(css) {
  return css.replace(/\/\*[\s\S]*?\*\//g, '');
}

function compactCss(css) {
  return stripCssComments(css)
    .replace(/\s+/g, ' ')
    .replace(/\s*([{}:;,>])\s*/g, '$1')
    .replace(/;}/g, '}')
    .trim();
}

function compactHtml(html) {
  // Only strip blank lines and leading/trailing whitespace per line; preserve content inside tags
  return html.replace(/^[ \t]+$/gm, '').trim();
}

function bundle() {
  fs.mkdirSync(outputDir, { recursive: true });

  const css = compactCss(readSource('styles.css'));
  const commandsJs = readSource('commands.js').trim();
  const parsersJs = readSource('parsers.js').trim();
  const js = readSource('app.js').trim();
  let html = fs.readFileSync(indexPath, 'utf8');

  html = html.replace(/<link\s+rel=["']?stylesheet["']?\s+href=["']?styles\.css["']?\s*>/i, () => `<style>${css}</style>`);
  html = html.replace(/<script\s+src=["']?commands\.js["']?\s*><\/script>/i, () => `<script>${commandsJs}</script>`);
  html = html.replace(/<script\s+src=["']?parsers\.js["']?\s*><\/script>/i, () => `<script>${parsersJs}</script>`);
  html = html.replace(/<script\s+src=["']?app\.js["']?\s*><\/script>/i, () => `<script>${js}</script>`);
  html = compactHtml(html);

  fs.writeFileSync(outputPath, `${html}\n`, 'utf8');

  const bytes = Buffer.byteLength(html);
  const gzBytes = zlib.gzipSync(Buffer.from(html), { level: 9 }).length;
  console.log(`Wrote ${path.relative(process.cwd(), outputPath)}`);
  console.log(`HTML size: ${bytes} bytes`);
  console.log(`Gzip size: ${gzBytes} bytes`);
}

bundle();
