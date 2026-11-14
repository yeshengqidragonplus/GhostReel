// AlphaClip preview: the page. Decoding happens in the local service (`alphaclip preview`, the native core); this
// file is the user interface: playback, drag and drop, backgrounds, zoom and the sidecar editor. The drawing is in
// renderer.js, the calls to the service in api.js.
import { Renderer, effectiveMatrix, effectiveFullRange } from './renderer.js';
import * as api from './api.js';

const query = new URLSearchParams(location.search);
const EN =query.has('lang') ? query.get('lang') === 'en' : !navigator.language.toLowerCase().startsWith('zh');
const tr = (zh, en) => (EN ? en : zh);
const $ = (id) => document.getElementById(id);

if (EN) {
  document.documentElement.lang = 'en';
  document.title = 'AlphaClip preview';
  for (const el of document.querySelectorAll('[data-en]')) el.textContent = el.dataset.en;
  for (const el of document.querySelectorAll('[data-en-title]')) el.title = el.dataset.enTitle;
}

const canvas = $('view');
const S = {
  renderer: null,
  clip: null,          // {id, name, size, meta, info, inspect, error}; info is null while the core refuses the clip
  metaOriginal: null,  // the sidecar text the clip came with ("Revert" goes back to it)
  metaBase: {},        // that sidecar as an object: keys the form does not know are kept
  cur: null,           // the frame on screen
  next: null,          // the frame after it (fetched ahead), 'end', or null while it is being fetched
  wrap: null,          // the first frame, fetched ahead when the clip is about to loop
  wrapAsked: false,
  gen: 0,              // bumped by every seek and clip change: answers to older requests are dropped
  playing: false,
  clockUs: 0,
  lastTick: 0,
  pan: [0, 0],
  rect: [0, 0, 0, 0],
  outputPixels: null,
  scrubbing: false,
};
window.__alphaclipPreview = S;   // read by uitest.js, the automatic test of this page

// ---- settings kept between visits ---------------------------------------------------------------------------------

const SETTINGS = ['opt-loop', 'opt-speed', 'bg-checker', 'bg-color', 'opt-zoom', 'opt-filter', 'opt-viewmode',
  'opt-colorspace', 'opt-blend', 'def-matrix', 'def-range', 'def-siting'];

function restoreSettings() {
  let saved = {};
  try {
    saved = JSON.parse(localStorage.getItem('alphaclip-preview') || '{}') || {};
  } catch (e) {
    saved = {};
  }
  for (const id of SETTINGS) {
    const el = $(id);
    if (!(id in saved)) continue;
    if (el.type === 'checkbox') {
      el.checked = !!saved[id];
    } else if (el.tagName !== 'SELECT' || Array.from(el.options).some((o) => o.value === saved[id])) {
      el.value = saved[id];
    }
  }
}

function saveSettings() {
  const out = {};
  for (const id of SETTINGS) out[id] = $(id).type === 'checkbox' ? $(id).checked : $(id).value;
  try {
    localStorage.setItem('alphaclip-preview', JSON.stringify(out));
  } catch (e) { /* private mode: nothing is kept */ }
}

// ---- messages -----------------------------------------------------------------------------------------------------

function showMessage(text) {
  $('message').hidden = !text;
  $('message').textContent = text || '';
}

function setMetaError(text) {
  $('meta-error').hidden = !text;
  $('meta-error').textContent = text || '';
}

// ---- drawing ------------------------------------------------------------------------------------------------------

function hexToRgb(hex) {
  const n = parseInt(hex.slice(1), 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}

function resizeCanvas() {
  const vp = $('viewport');
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.round(vp.clientWidth * dpr)), h = Math.max(1, Math.round(vp.clientHeight * dpr));
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
}

function redraw() {
  const r = S.renderer;
  if (!r) return;
  resizeCanvas();
  const linear = $('opt-colorspace').value === 'linear';
  const info = S.clip && S.clip.info;
  if (info && S.cur) {
    r.drawOutput({
      matrix: effectiveMatrix(info, $('def-matrix').value),
      fullRange: effectiveFullRange(info, $('def-range').value === 'full'),
      chromaShift: $('def-siting').value === 'left' ? 0.25 : 0, linear,
    });
    S.outputPixels = null;
  }
  const W = canvas.width, H = canvas.height;
  let rect = [0, 0, 0, 0];
  if (info) {
    // 100% = one pixel of the picture on one pixel of the screen.
    const pw = info.content_w, ph = info.content_h;
    const zoom = $('opt-zoom').value === 'fit' ? Math.min(W / pw, H / ph) : parseFloat($('opt-zoom').value);
    const dw = Math.max(1, Math.round(pw * zoom)), dh = Math.max(1, Math.round(ph * zoom));
    const x0 = Math.round((W - dw) / 2 + S.pan[0]), y0 = Math.round((H - dh) / 2 + S.pan[1]);
    rect = [x0, y0, x0 + dw, y0 + dh];
  }
  S.rect = rect;
  r.drawScene({
    width: W, height: H, rect, linear,
    filter: $('opt-filter').value, viewMode: Number($('opt-viewmode').value), blend: $('opt-blend').value,
    background: $('bg-checker').checked
      ? { color: [96, 96, 96], checker: { color: [144, 144, 144], size: 8 } }
      : { color: hexToRgb($('bg-color').value), checker: null },
  });
  r.present();
}

function updateReadout(ev) {
  const info = S.clip && S.clip.info;
  const out = $('readout');
  if (!info || !S.cur || !ev) {
    out.textContent = ' ';
    return;
  }
  const box = canvas.getBoundingClientRect();
  const scale = canvas.width / box.width;
  const [x0, y0, x1, y1] = S.rect;
  const px = Math.floor(((ev.clientX - box.left) * scale - x0) / (x1 - x0) * info.content_w);
  const py = Math.floor(((ev.clientY - box.top) * scale - y0) / (y1 - y0) * info.content_h);
  if (px < 0 || py < 0 || px >= info.content_w || py >= info.content_h) {
    out.textContent = ' ';
    return;
  }
  if (!S.outputPixels) S.outputPixels = S.renderer.readOutput();
  const d = S.outputPixels.data, o = (py * S.outputPixels.width + px) * 4;
  out.textContent = 'x ' + px + '  y ' + py + '   ' + tr('着色器输出(预乘 RGBA)', 'shader output (premultiplied RGBA)') +
    '  ' + d[o] + ' ' + d[o + 1] + ' ' + d[o + 2] + ' ' + d[o + 3];
}

// ---- frames -------------------------------------------------------------------------------------------------------

// One request to the service at a time; a newer request replaces one that has not started yet.
let pending = null;
let busy = false;

function request(queryObject, done) {
  pending = { query: queryObject, gen: S.gen, clipId: S.clip.id, done };
  pump();
}

async function pump() {
  if (busy) return;
  busy = true;
  while (pending) {
    const job = pending;
    pending = null;
    try {
      const frame = await api.fetchFrame(job.clipId, job.query);
      if (job.gen === S.gen) job.done(frame);
    } catch (e) {
      if (job.gen === S.gen) {
        showMessage(e.message);
        setPlaying(false);
      }
    }
  }
  busy = false;
}

function frameUs() {
  const info = S.clip && S.clip.info;
  return info && info.fps > 0 ? 1e6 / info.fps : 33333;
}

function show(frame) {
  S.cur = frame;
  S.next = null;
  S.wrap = null;
  S.wrapAsked = false;
  S.renderer.uploadFrame(frame);
  redraw();
  updateTransport();
  request({ after_us: frame.pts }, (next) => {
    S.next = next || 'end';
  });
}

function seekTo(us) {
  if (!S.clip || !S.clip.info) return;
  S.gen++;
  S.next = null;
  S.wrap = null;
  S.wrapAsked = false;
  request({ at_us: Math.max(0, Math.round(us)) }, (frame) => {
    if (!frame) return;
    S.clockUs = frame.pts;
    show(frame);
  });
}

function stepNext() {
  setPlaying(false);
  if (S.next && S.next !== 'end') {
    S.clockUs = S.next.pts;
    show(S.next);
  } else if (S.next === 'end' && $('opt-loop').checked) {
    seekTo(0);
  }
}

function stepPrevious() {
  setPlaying(false);
  if (S.cur) seekTo(S.cur.pts - 1);
}

function setPlaying(on) {
  S.playing = !!on && !!(S.clip && S.clip.info);
  $('btn-play').textContent = S.playing ? '⏸' : '▶';
}

function tick(now) {
  requestAnimationFrame(tick);
  const dt = Math.min(250, now - S.lastTick);
  S.lastTick = now;
  if (!S.playing || !S.cur) return;
  S.clockUs += dt * 1000 * Number($('opt-speed').value);
  const period = frameUs();
  if (S.next && S.next !== 'end') {
    if (S.clockUs >= S.next.pts) {
      if (S.clockUs - S.next.pts > 4 * period) S.clockUs = S.next.pts;   // fell behind: do not chase
      show(S.next);
    }
  } else if (S.next === 'end') {
    const loop = $('opt-loop').checked;
    if (loop && !S.wrapAsked) {
      S.wrapAsked = true;
      request({ at_us: 0 }, (first) => {
        S.wrap = first;
      });
    }
    if (S.clockUs >= S.cur.pts + period) {
      if (!loop) {
        setPlaying(false);
      } else if (S.wrap) {
        S.clockUs = S.wrap.pts + Math.min(period, S.clockUs - (S.cur.pts + period));
        show(S.wrap);
      } else {
        S.clockUs = S.cur.pts + period;   // the first frame is not here yet: wait for it
      }
    }
  } else {
    S.clockUs = Math.min(S.clockUs, S.cur.pts + period);   // the next frame is not here yet: wait for it
  }
}

function updateTransport() {
  const info = S.clip && S.clip.info;
  if (!info || !S.cur) {
    $('time-label').textContent = '–';
    return;
  }
  const total = Math.max(info.duration_us, info.loop_period_us, 1);
  $('timeline').max = String(total);
  if (!S.scrubbing) $('timeline').value = String(S.cur.pts);
  const seconds = (us) => (us / 1e6).toFixed(3);
  let text = seconds(S.cur.pts) + ' / ' + seconds(total) + ' s';
  if (info.fps > 0) {
    text = tr('帧 ', 'frame ') + Math.round(S.cur.pts * info.fps / 1e6) + ' / ' + info.packet_count + '   ' + text;
  }
  $('time-label').textContent = text;
}

// ---- clips --------------------------------------------------------------------------------------------------------

function updateClipUi() {
  const clip = S.clip;
  $('clip-name').textContent = clip ? clip.name : tr('没有', 'none');
  $('clip-name').title = clip ? clip.name : '';
  $('clip-name').classList.toggle('muted', !clip);
  $('inspect').textContent = clip ? clip.inspect || '' : '';
  $('drop-hint').hidden = !!(clip && clip.info);
}

// The core accepted the clip (first time, or again with other metadata): textures, then the frame at `us`.
function clipOpened(us) {
  S.gen++;
  pending = null;
  S.cur = null;
  S.next = null;
  S.wrap = null;
  S.renderer.setClip(S.clip.info);
  updateClipUi();
  showMessage(null);
  redraw();
  seekTo(us);
}

// Opens the current clip with a sidecar text (null: none). False when the core refuses: the picture stays as it was.
async function openWith(metaText) {
  const clip = S.clip;
  if (!clip) return false;
  try {
    const r = await api.openClip(clip.id, metaText);
    if (S.clip !== clip) return false;
    Object.assign(clip, { meta: r.meta, info: r.info, inspect: r.inspect, error: null });
    setMetaError(null);
    clipOpened(S.cur ? S.cur.pts : 0);
    return true;
  } catch (e) {
    if (S.clip === clip) setMetaError(e.message + (e.status ? '  (' + e.status + ')' : ''));
    return false;
  }
}

async function loadFiles(files) {
  const list = Array.from(files);
  const json = list.find((f) => /\.json$/i.test(f.name));
  const webm = list.find((f) => /\.webm$/i.test(f.name)) || list.find((f) => f !== json);
  try {
    if (!webm) {
      if (!json || !S.clip) {
        showMessage(tr('请拖入一个 .webm 文件。', 'Drop a .webm file.'));
        return;
      }
      // A sidecar alone: it replaces the current clip's.
      S.metaOriginal = await json.text();
      fillForm(S.metaOriginal);
      await openWith(S.metaOriginal);
      return;
    }
    showMessage(null);
    const up = await api.uploadClip(webm.name, await webm.arrayBuffer());
    // The new clip replaces the old one at once, whether or not the core can open it.
    setPlaying(false);
    S.gen++;
    pending = null;
    S.clip = { id: up.id, name: up.name, size: up.size, meta: null, info: null, inspect: '', error: null };
    S.metaOriginal = json ? await json.text() : null;
    S.cur = null;
    S.next = null;
    S.pan = [0, 0];
    S.renderer.forget();
    setMetaError(null);
    updateClipUi();
    updateTransport();
    redraw();
    const ok = await openWith(S.metaOriginal);
    fillForm(S.metaOriginal);
    if (ok) setPlaying(true);
  } catch (e) {
    showMessage(e.message);
  }
}

// ---- the sidecar editor -------------------------------------------------------------------------------------------

const MODE_NAMES = ['opaque', 'packed_right', 'packed_bottom', 'native_alpha'];

// Puts a sidecar text into the form. Without one the fields show what the core's auto mode chose.
function fillForm(metaText) {
  const info = S.clip && S.clip.info;
  let obj = null;
  if (metaText) {
    try {
      obj = JSON.parse(metaText);
    } catch (e) {
      obj = null;
    }
    if (obj === null || typeof obj !== 'object' || Array.isArray(obj)) obj = {};
  }
  S.metaBase = obj || {};
  $('meta-use').checked = !!metaText;
  $('meta-fields').disabled = !metaText;
  const o = obj || {};
  const mode = typeof o.alpha_mode === 'string' && MODE_NAMES.includes(o.alpha_mode) ? o.alpha_mode
    : info ? MODE_NAMES[info.alpha_mode] || 'opaque' : 'opaque';
  $('meta-alpha-mode').value = mode;
  $('meta-premultiplied').checked = obj ? o.premultiplied === true : !!(info && info.premultiplied);
  $('meta-matrix').value = o.matrix === 'bt601' || o.matrix === 'bt709' ? o.matrix : '';
  $('meta-range').value = o.color_range === 'limited' || o.color_range === 'full' ? o.color_range : '';
  const size = Array.isArray(o.content_size) && o.content_size.length === 2 ? o.content_size : ['', ''];
  $('meta-content-w').value = size[0];
  $('meta-content-h').value = size[1];
  $('meta-loop-hint').checked = o.loop_hint === true;
  $('meta-json').textContent = metaText || tr('(没有 sidecar)', '(no sidecar)');
}

// The form as sidecar text; null when "use a sidecar" is off.
function formToText() {
  if (!$('meta-use').checked) return null;
  const base = S.metaBase || {};
  const out = { schema: 1, alpha_mode: $('meta-alpha-mode').value, premultiplied: $('meta-premultiplied').checked };
  if ($('meta-range').value) out.color_range = $('meta-range').value;
  if ($('meta-matrix').value) out.matrix = $('meta-matrix').value;
  const w = parseInt($('meta-content-w').value, 10), h = parseInt($('meta-content-h').value, 10);
  if (w > 0 && h > 0) out.content_size = [w, h];
  if ($('meta-loop-hint').checked || 'loop_hint' in base) out.loop_hint = $('meta-loop-hint').checked;
  const known = ['schema', 'alpha_mode', 'premultiplied', 'color_range', 'matrix', 'content_size', 'loop_hint'];
  for (const key of Object.keys(base)) {
    if (!known.includes(key)) out[key] = base[key];
  }
  return JSON.stringify(out, null, 2) + '\n';
}

async function formChanged() {
  $('meta-fields').disabled = !$('meta-use').checked;
  const text = formToText();
  $('meta-json').textContent = text || tr('(没有 sidecar)', '(no sidecar)');
  if (S.clip) await openWith(text);
}

function sidecarName() {
  const name = S.clip ? S.clip.name : 'clip.webm';
  const dot = name.lastIndexOf('.');
  return (dot > 0 ? name.slice(0, dot) : name) + '.acmeta.json';
}

// ---- wiring -------------------------------------------------------------------------------------------------------

function wire() {
  for (const id of SETTINGS) {
    $(id).addEventListener('change', () => {
      saveSettings();
      redraw();
    });
  }
  $('bg-color').addEventListener('input', () => {
    $('bg-checker').checked = false;
    redraw();
  });
  for (const b of $('bg-presets').querySelectorAll('button')) {
    b.addEventListener('click', () => {
      $('bg-color').value = b.dataset.color;
      $('bg-checker').checked = false;
      saveSettings();
      redraw();
    });
  }
  $('opt-zoom').addEventListener('change', () => {
    S.pan = [0, 0];
    redraw();
  });

  $('btn-open').addEventListener('click', () => $('file-input').click());
  $('file-input').addEventListener('change', (ev) => {
    loadFiles(ev.target.files);
    ev.target.value = '';
  });
  $('btn-play').addEventListener('click', () => {
    if (!S.playing && S.next === 'end' && !$('opt-loop').checked) seekTo(0);
    setPlaying(!S.playing);
  });
  $('btn-next').addEventListener('click', stepNext);
  $('btn-prev').addEventListener('click', stepPrevious);
  $('timeline').addEventListener('pointerdown', () => { S.scrubbing = true; });
  $('timeline').addEventListener('pointerup', () => { S.scrubbing = false; });
  $('timeline').addEventListener('input', (ev) => seekTo(Number(ev.target.value)));

  for (const id of ['meta-use', 'meta-alpha-mode', 'meta-premultiplied', 'meta-matrix', 'meta-range',
    'meta-content-w', 'meta-content-h', 'meta-loop-hint']) {
    $(id).addEventListener('change', formChanged);
  }
  $('meta-reset').addEventListener('click', async () => {
    fillForm(S.metaOriginal);
    if (S.clip) await openWith(S.metaOriginal);
  });
  $('meta-copy').addEventListener('click', async () => {
    const text = formToText();
    if (text) await navigator.clipboard.writeText(text);
  });
  $('meta-download').addEventListener('click', () => {
    const text = formToText();
    if (!text) return;
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([text], { type: 'application/json' }));
    a.download = sidecarName();
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  });

  // Drag and drop anywhere on the page.
  const vp = $('viewport');
  document.addEventListener('dragover', (ev) => {
    ev.preventDefault();
    vp.classList.add('drop');
  });
  document.addEventListener('dragleave', () => vp.classList.remove('drop'));
  document.addEventListener('drop', (ev) => {
    ev.preventDefault();
    vp.classList.remove('drop');
    if (ev.dataTransfer && ev.dataTransfer.files.length) loadFiles(ev.dataTransfer.files);
  });

  // Pan with the mouse, zoom with the wheel, the pixel under the pointer.
  let drag = null;
  canvas.addEventListener('pointerdown', (ev) => {
    drag = { x: ev.clientX, y: ev.clientY, pan: S.pan.slice() };
    canvas.classList.add('dragging');
    try {
      canvas.setPointerCapture(ev.pointerId);
    } catch (e) { /* a pointer the browser does not know (a scripted event): dragging works without the capture */ }
  });
  canvas.addEventListener('pointermove', (ev) => {
    if (drag) {
      const scale = canvas.width / canvas.getBoundingClientRect().width;
      S.pan = [drag.pan[0] + (ev.clientX - drag.x) * scale, drag.pan[1] + (ev.clientY - drag.y) * scale];
      redraw();
    }
    updateReadout(ev);
  });
  const endDrag = () => {
    drag = null;
    canvas.classList.remove('dragging');
  };
  canvas.addEventListener('pointerup', endDrag);
  canvas.addEventListener('pointercancel', endDrag);
  canvas.addEventListener('pointerleave', () => updateReadout(null));
  canvas.addEventListener('dblclick', () => {
    S.pan = [0, 0];
    redraw();
  });
  canvas.addEventListener('wheel', (ev) => {
    ev.preventDefault();
    const sel = $('opt-zoom');
    const steps = Array.from(sel.options).map((o) => o.value).filter((v) => v !== 'fit');
    let i = steps.indexOf(sel.value);
    if (i < 0) i = steps.indexOf('1');
    i = Math.max(0, Math.min(steps.length - 1, i + (ev.deltaY < 0 ? 1 : -1)));
    sel.value = steps[i];
    saveSettings();
    redraw();
  }, { passive: false });

  document.addEventListener('keydown', (ev) => {
    if (['INPUT', 'SELECT', 'TEXTAREA', 'BUTTON'].includes(ev.target.tagName)) return;
    if (ev.key === ' ') {
      ev.preventDefault();
      $('btn-play').click();
    } else if (ev.key === 'ArrowRight') {
      stepNext();
    } else if (ev.key === 'ArrowLeft') {
      stepPrevious();
    }
  });

  new ResizeObserver(redraw).observe($('viewport'));
}

async function main() {
  restoreSettings();
  try {
    const res = await fetch('shaders/alphaclip_yuv_to_rgba.frag.glsl');
    if (!res.ok) throw new Error('cannot load the shader (HTTP ' + res.status + ')');
    S.renderer = new Renderer(canvas, await res.text());
  } catch (e) {
    showMessage(String(e && e.message ? e.message : e));
    return;
  }
  wire();
  fillForm(null);
  updateClipUi();
  redraw();
  try {
    const state = await api.getState();
    if (state.clip) {
      S.clip = state.clip;
      S.metaOriginal = state.clip.meta;
      fillForm(state.clip.meta);
      if (state.clip.info) {
        clipOpened(0);
        setPlaying(true);
      } else {
        updateClipUi();
        setMetaError(state.clip.error);
        showMessage(state.clip.error);
      }
    }
  } catch (e) {
    showMessage(e.message);
  }
  S.lastTick = performance.now();
  requestAnimationFrame(tick);
}

main();
