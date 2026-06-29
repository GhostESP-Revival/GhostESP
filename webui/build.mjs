import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';

const root = path.dirname(new URL(import.meta.url).pathname).replace(/^\/(.:)/, '$1');
const projectRoot = path.resolve(root, '..');
const sourceDir = path.join(root, 'src');
const indexPath = path.join(sourceDir, 'index.html');
const outputDir = path.join(root, 'dist');
const outputPath = path.join(outputDir, 'ghost_site.html');

const DEFAULT_LOGO_REL = path.join(
  'GhostESP_Brand_Assets',
  '01_logo',
  'png',
  'ghostesp_logo_white_transparent.png'
);

function parseArgs(argv) {
  const args = { logo: null, noInline: false };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--logo' && argv[i + 1]) {
      args.logo = argv[++i];
    } else if (a.startsWith('--logo=')) {
      args.logo = a.slice('--logo='.length);
    } else if (a === '--no-inline-logo') {
      args.noInline = true;
    }
  }
  return args;
}

function resolveLogoPath(argLogo, htmlSrc) {
  const candidates = [
    argLogo,
    process.env.BRAND_LOGO,
    path.join(root, DEFAULT_LOGO_REL),
    path.join(projectRoot, DEFAULT_LOGO_REL),
  ];
  if (htmlSrc && !/^data:/i.test(htmlSrc)) {
    candidates.push(path.resolve(sourceDir, htmlSrc));
    candidates.push(path.resolve(projectRoot, htmlSrc));
  }
  for (const candidate of candidates) {
    if (!candidate) continue;
    try {
      const abs = path.resolve(candidate);
      if (fs.existsSync(abs) && fs.statSync(abs).isFile()) return abs;
    } catch (_) {
      /* ignore */
    }
  }
  return null;
}

function inlineLogo(html, options) {
  const re = /<img([^>]*?)class="brand-logo"([^>]*?)src="([^"]*)"/i;
  const m = html.match(re);
  if (!m) return html;

  const htmlSrc = m[3];
  if (options.noInline) {
    console.log('[build] --no-inline-logo set; keeping src attribute as-is for dist.');
    return html;
  }

  const logoPath = resolveLogoPath(options.logo, htmlSrc);
  if (!logoPath) {
    console.warn('[build] brand logo not found; leaving src attribute as-is.');
    console.warn('[build] set BRAND_LOGO env var or pass --logo <path>.');
    return html;
  }

  const buf = fs.readFileSync(logoPath);
  const mime = 'image/png';
  const dataUri = `data:${mime};base64,${buf.toString('base64')}`;
  console.log(`[build] inlined logo: ${path.relative(projectRoot, logoPath)} (${buf.length} bytes)`);

  return html.replace(re, (_full, before, after) => {
    return `<img${before}class="brand-logo"${after}src="${dataUri}"`;
  });
}

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
  return html.replace(/^[ \t]+$/gm, '').trim();
}

function bundle() {
  fs.mkdirSync(outputDir, { recursive: true });

  const options = parseArgs(process.argv);

  const css = compactCss(readSource('styles.css'));
  const commandsJs = readSource('commands.js').trim();
  const parsersJs = readSource('parsers.js').trim();
  const js = readSource('app.js').trim();
  let html = fs.readFileSync(indexPath, 'utf8');
  html = inlineLogo(html, options);

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
