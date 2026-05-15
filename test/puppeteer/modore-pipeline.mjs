/**
 * Fully automated E2E: spawn modore-host (optional) + Puppeteer Chromium + --trigger.
 *
 * Default: spawns `modore-host --ipc-only` with MODORE_IPC_SOCKET under /tmp so it does not
 * collide with systemd; leaves your real XDG_RUNTIME_DIR (Hypr/wtype need the Wayland socket).
 * No manual typing or pre-running the daemon.
 *
 * Env:
 *   MODORE_HOST_BIN     Path to modore-host (default: repo native/linux/build, then PATH)
 *   MODORE_SPAWN_HOST=0 Do not spawn — use running daemon (unset MODORE_IPC_SOCKET or match it)
 *   MODORE_IPC_SOCKET   Override socket path (both host + trigger; tests set this automatically)
 *   MODORE_ROMANJI      Test word (default: nihongo)
 *   MODORE_PUPPETEER_SESSION=1  Use native Ozone Wayland in Chromium (production-like; do_pickup
 *                        nullptr). Default is XWayland — THAT IS NOT THE SAME as a normal Hypr
 *                        Chromium tab, which is usually native Wayland.
 *   MODORE_PUPPETEER_X11=1      Force Ozone X11 (redundant with default when SESSION is unset).
 *   PUPPETEER_EXECUTABLE_PATH   System chromium often behaves better for SESSION=1 tests.
 *   MODORE_PUPPETEER_SCREENSHOT
 *   MODORE_E2E_TRACE=0   Disable host [e2e] logs (npm test sets MODORE_E2E_TRACE=1)
 */

import { spawn, spawnSync } from 'node:child_process';
import { existsSync, unlinkSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { basename, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import puppeteer from 'puppeteer-core';

const __dirname = dirname(fileURLToPath(import.meta.url));
const repoRoot = join(__dirname, '..', '..');

function e2eStep(msg, detail = '') {
  const d = detail ? ` | ${detail}` : '';
  console.info(`[e2e-test] ${msg}${d}`);
}

let hostChild = null;
/** Set when we own the test socket file. */
let testSocketPath = null;

function cleanup() {
  if (hostChild && !hostChild.killed) {
    try {
      hostChild.kill('SIGTERM');
    } catch {}
    hostChild = null;
  }
  if (testSocketPath && existsSync(testSocketPath)) {
    try {
      unlinkSync(testSocketPath);
    } catch {}
    testSocketPath = null;
  }
}

for (const sig of ['SIGINT', 'SIGTERM']) {
  process.on(sig, () => {
      cleanup();
      process.exit(130);
    });
}
process.on('exit', cleanup);

function resolveModoreHost() {
  const envPath = process.env.MODORE_HOST_BIN;
  if (envPath && existsSync(envPath)) {
    return envPath;
  }
  const build = join(repoRoot, 'native', 'linux', 'build', 'modore-host');
  if (existsSync(build)) {
    return build;
  }
  const which = spawnSync('which', ['modore-host'], { encoding: 'utf8' });
  if (which.status === 0) {
    const p = which.stdout.trim().split('\n')[0];
    if (p && existsSync(p)) {
      return p;
    }
  }
  const localBin = join(process.env.HOME || '', '.local', 'bin', 'modore-host');
  if (existsSync(localBin)) {
    return localBin;
  }
  return null;
}

function resolveBrowserExecutable() {
  const envPath = process.env.PUPPETEER_EXECUTABLE_PATH;
  if (envPath && existsSync(envPath)) {
    return envPath;
  }

  const pathCandidates = [
    '/usr/bin/chromium',
    '/usr/bin/chromium-browser',
    '/usr/bin/google-chrome',
    '/opt/google/chrome/chrome',
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  ];
  for (const candidate of pathCandidates) {
    if (existsSync(candidate)) return candidate;
  }

  for (const cmd of ['chromium', 'chromium-browser', 'google-chrome', 'chrome']) {
    const which = spawnSync('which', [cmd], { encoding: 'utf8' });
    if (which.status === 0) {
      const p = which.stdout.trim().split('\n')[0];
      if (p && existsSync(p)) return p;
    }
  }
  return null;
}

/** Host cwd + env so dlopen finds libmozc_bridge.so next to the binary in build/. */
function hostSpawnEnv(bin) {
  const env = { ...process.env };
  const dir = dirname(bin);
  if (basename(dir) === 'build' && dir.includes('native/linux')) {
    env.LD_LIBRARY_PATH = [dir, env.LD_LIBRARY_PATH || ''].filter(Boolean).join(':');
  }
  return env;
}

async function waitForSocket(sockPath, timeoutMs) {
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    if (existsSync(sockPath)) {
      return;
    }
    await new Promise((r) => setTimeout(r, 50));
  }
  throw new Error(`timeout waiting for ${sockPath}`);
}

function runTrigger(bin, env) {
  const r = spawnSync(bin, ['--trigger'], { encoding: 'utf8', env });
  return { ok: r.status === 0, status: r.status, stderr: r.stderr || '', stdout: r.stdout || '' };
}

function focusWindowTitleContains(token) {
  const hypr = spawnSync('hyprctl', ['-j', 'clients'], { encoding: 'utf8' });
  if (hypr.status === 0 && hypr.stdout) {
    try {
      const clients = JSON.parse(hypr.stdout);
      for (const c of clients) {
        if (c.title && String(c.title).includes(token)) {
          const r = spawnSync('hyprctl', ['dispatch', 'focuswindow', `address:${c.address}`], {
            encoding: 'utf8',
          });
          if (r.status === 0) {
            console.info('modore-pipeline: hyprctl focuswindow', c.address, c.title);
            return true;
          }
        }
      }
    } catch {}
  }
  const xd = spawnSync('xdotool', ['search', '--name', token, 'windowactivate'], { encoding: 'utf8' });
  if (xd.status === 0) {
    console.info('modore-pipeline: xdotool windowactivate', token);
    return true;
  }
  return false;
}

function hasJapanese(s) {
  return /[\u3000-\u303f\u3040-\u30ff\u3400-\u9fff\uff00-\uffef]/.test(s);
}

const modoreBin = resolveModoreHost();
if (!modoreBin) {
  console.error('modore-pipeline: could not find modore-host (build native/linux first or set MODORE_HOST_BIN)');
  process.exit(1);
}

console.info('modore-pipeline: using', modoreBin);
e2eStep('resolved MODORE_HOST_BIN', modoreBin);

const spawnHost = process.env.MODORE_SPAWN_HOST !== '0';

if (spawnHost) {
  testSocketPath = join(tmpdir(), `modore-e2e-${process.pid}.sock`);
  process.env.MODORE_IPC_SOCKET = testSocketPath;
  console.info('modore-pipeline: MODORE_IPC_SOCKET=', testSocketPath);
  e2eStep('isolated IPC socket', testSocketPath);
}

const hostEnv = hostSpawnEnv(modoreBin);
Object.assign(process.env, hostEnv);
const mergedEnv = { ...process.env, ...hostEnv };

// Default browser mode is Ozone X11 (XWayland): `run_ipc_pickup` opens an X11 Display* so pickup
// uses XTest + X11 CLIPBOARD — this passed reliably with Puppeteer's Chromium.
//
// Real Chromium on Hypr is often native Ozone Wayland: focused window is not xwayland, so
// production uses do_pickup(nullptr) + Hypr/wtype + wl-clipboard — a different code path.
// Set MODORE_PUPPETEER_SESSION=1 to mimic that (may fail with Chrome-for-Testing until clipboard
// behaviour matches; try PUPPETEER_EXECUTABLE_PATH=/usr/bin/chromium).
const sessionWayland =
  !!(mergedEnv.WAYLAND_DISPLAY && String(mergedEnv.WAYLAND_DISPLAY).trim());
const mimicSessionChrome = process.env.MODORE_PUPPETEER_SESSION === '1';
const forceX11 = process.env.MODORE_PUPPETEER_X11 === '1';
const useX11Chrome =
  forceX11 || !sessionWayland || !mimicSessionChrome;

e2eStep(
  'env for host/trigger',
  `MODORE_E2E_TRACE=${mergedEnv.MODORE_E2E_TRACE ?? '(unset)'} MODORE_IPC_SOCKET=${mergedEnv.MODORE_IPC_SOCKET ?? '(unset)'} chrome=${useX11Chrome ? 'x11-harness' : 'wayland-session'}`,
);

if (spawnHost) {
  const hostCwd = dirname(modoreBin);
  e2eStep('spawn modore-host --ipc-only', `cwd=${hostCwd}`);
  hostChild = spawn(modoreBin, ['--ipc-only'], {
    cwd: hostCwd,
    env: mergedEnv,
    stdio: ['ignore', 'inherit', 'inherit'],
  });
  const sockPath = mergedEnv.MODORE_IPC_SOCKET;
  try {
    await waitForSocket(sockPath, 10_000);
  } catch (e) {
    console.error(String(e));
    process.exit(1);
  }
  // Socket path exists once bind() completed; listen() follows immediately in ipc thread.
  await new Promise((r) => setTimeout(r, 80));
  console.info('modore-pipeline: modore-host --ipc-only ready');
  e2eStep('host IPC listening', sockPath);
} else {
  e2eStep('MODORE_SPAWN_HOST=0', 'preflight trigger only');
  const t0 = runTrigger(modoreBin, mergedEnv);
  if (!t0.ok) {
    console.error(
      'modore-pipeline: `modore-host --trigger` failed — is the host running?\n',
      t0.stderr,
      t0.stdout,
    );
    process.exit(1);
  }
}

const romanji = process.env.MODORE_ROMANJI || 'nihongo';
const headless = process.env.MODORE_PUPPETEER_HEADLESS === '1' ? 'shell' : false;
const titleToken = `modore-e2e-${process.pid}`;
const browserExecutable = resolveBrowserExecutable();

if (!browserExecutable) {
  console.error(
    'modore-pipeline: could not find a Chromium/Chrome executable; set PUPPETEER_EXECUTABLE_PATH',
  );
  process.exit(1);
}

const browserEnv = { ...mergedEnv };
const launchArgs = ['--no-sandbox', '--disable-setuid-sandbox', '--window-size=720,480'];
if (useX11Chrome) {
  if (browserEnv.DISPLAY && browserEnv.DISPLAY[0]) {
    browserEnv.OZONE_PLATFORM = 'x11';
    delete browserEnv.WAYLAND_DISPLAY;
  }
  launchArgs.push('--ozone-platform=x11');
} else {
  launchArgs.push('--ozone-platform=wayland');
}

e2eStep(
  'browser chrome mode',
  useX11Chrome
    ? 'Ozone X11 / XWayland (default) — do_pickup(Display*) same as focused xwayland: true window'
    : 'Ozone Wayland (MODORE_PUPPETEER_SESSION=1) — do_pickup(nullptr) like typical Chromium tab',
);
e2eStep('browser env', `OZONE_PLATFORM=${browserEnv.OZONE_PLATFORM ?? '(unset)'} WAYLAND_DISPLAY=${browserEnv.WAYLAND_DISPLAY ?? '(unset)'}`);
e2eStep('puppeteer.launch', `headless=${String(headless)} exe=${browserExecutable} args=${launchArgs.join(' ')}`);

const browser = await puppeteer.launch({
  headless,
  args: launchArgs,
  executablePath: browserExecutable,
  env: browserEnv,
});

try {
  const page = await browser.newPage();
  e2eStep('page loaded', `romanji=${romanji} titleToken=${titleToken}`);
  const html = `<!doctype html>
<html><head><meta charset="utf-8"><title>${titleToken}</title></head>
<body style="margin:2rem;font:16px sans-serif">
  <input id="f" type="text" size="40" autocomplete="off" spellcheck="false" value="" autofocus />
</body></html>`;
  await page.goto(`data:text/html,${encodeURIComponent(html)}`, { waitUntil: 'domcontentloaded' });
  await page.evaluate((t) => {
    document.title = t;
  }, titleToken);

  const inputSel = '#f';
  e2eStep('DOM', 'focus input, type, Ctrl+A');
  await page.bringToFront();
  await page.focus(inputSel);
  await page.keyboard.type(romanji, { delay: 12 });
  await page.keyboard.down('Control');
  await page.keyboard.press('a');
  await page.keyboard.up('Control');
  await new Promise((r) => setTimeout(r, 50));

  await page.bringToFront();
  e2eStep('focus browser window', titleToken);
  if (!focusWindowTitleContains(titleToken)) {
    console.warn('modore-pipeline: could not Hypr/xdotool focus — continuing (may be flaky)');
  }
  await new Promise((r) => setTimeout(r, 50));

  e2eStep('modore-host --trigger (pickup + inject)');
  const tr = runTrigger(modoreBin, mergedEnv);
  if (!tr.ok) {
    console.error('modore-pipeline: trigger failed', tr.status, tr.stderr, tr.stdout);
    process.exit(1);
  }
  e2eStep('trigger ok', 'poll input for Japanese');

  const deadline = Date.now() + 15_000;
  let value = '';
  while (Date.now() < deadline) {
    value = await page.$eval(inputSel, (el) => el.value);
    if (value !== romanji && hasJapanese(value)) {
      console.info('modore-pipeline: OK →', JSON.stringify(value));
      e2eStep('PASS', JSON.stringify(value));
      process.exit(0);
    }
    await new Promise((r) => setTimeout(r, 40));
  }

  console.error(
    'modore-pipeline: timeout — value=%s (want Japanese, not %s). See host log above or ~/.config/modore/modore.log',
    JSON.stringify(value),
    JSON.stringify(romanji),
  );
  e2eStep('FAIL', `value=${JSON.stringify(value)}`);
  if (process.env.MODORE_PUPPETEER_SCREENSHOT) {
    await page.screenshot({ path: process.env.MODORE_PUPPETEER_SCREENSHOT }).catch(() => {});
    console.error('modore-pipeline: screenshot', process.env.MODORE_PUPPETEER_SCREENSHOT);
  }
  process.exit(1);
} finally {
  await browser.close();
}
