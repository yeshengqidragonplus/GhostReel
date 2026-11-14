// AlphaClip preview: a test of the page itself (core/tests/test_preview.py, part D, opens
// uitest.html?sink=http://127.0.0.1:<port>). The real page runs in a frame, started with a clip on the command line;
// this script uses it the way a person would (play, pause, step, seek, loop, change the sidecar, drop files, zoom) and
// posts what it saw to the test's collector:
//   GET  <sink>/file/<name>                           clips and sidecars to drop on the page
//   POST <sink>/result?name=N&width=W&height=H&pts=P  pixels the page drew (RGBA, rows top first)
//   POST <sink>/done                                  {"ui": [{ok, label, detail}], "errors": [...], "fatal"}
const sink = new URLSearchParams(location.search).get('sink') || '';
const frame = document.getElementById('app');
const win = () => frame.contentWindow;
const $ = (id) => frame.contentDocument.getElementById(id);
const S = () => win().__alphaclipPreview;   // the page's state (app.js)
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const report = { ui: [], errors: [], fatal: null };
const check = (ok, label, detail) => {
  report.ui.push({ ok: !!ok, label, detail: ok ? '' : String(detail === undefined ? '' : detail) });
  document.getElementById('log').textContent += (ok ? 'PASS  ' : 'FAIL  ') + label + '\n';
};

async function until(condition, ms = 8000) {
  const start = performance.now();
  while (performance.now() - start < ms) {
    try {
      if (condition()) return true;
    } catch (e) { /* not there yet */ }
    await sleep(15);
  }
  return false;
}

function set(id, value) {
  const el = $(id);
  if (el.type === 'checkbox') el.checked = value;
  else el.value = value;
  el.dispatchEvent(new (win().Event)('change', { bubbles: true }));
}

async function post(path, body, type) {
  await fetch(sink + path, { method: 'POST', headers: { 'Content-Type': type }, body });
}

// The part of the scene the picture covers, as the page drew it.
async function sendPicture(name) {
  const s = S();
  const scene = s.renderer.readScene();
  const [x0, y0, x1, y1] = s.rect;
  const w = x1 - x0, h = y1 - y0;
  const out = new Uint8Array(w * h * 4);
  for (let r = 0; r < h; r++) {
    out.set(scene.data.subarray(((y0 + r) * scene.width + x0) * 4, ((y0 + r) * scene.width + x1) * 4), r * w * 4);
  }
  await post('/result?' + new URLSearchParams({ name, width: w, height: h, pts: s.cur.pts }), out, 'application/octet-stream');
  return scene;
}

async function drop(names) {
  const dt = new (win().DataTransfer)();
  for (const name of names) {
    const bytes = await (await fetch(sink + '/file/' + encodeURIComponent(name))).arrayBuffer();
    dt.items.add(new (win().File)([bytes], name));
  }
  frame.contentDocument.dispatchEvent(new (win().DragEvent)('drop', { dataTransfer: dt, bubbles: true, cancelable: true }));
}

async function run() {
  if (!/^http:\/\/127\.0\.0\.1:\d{1,5}$/.test(sink)) throw new Error('give ?sink=http://127.0.0.1:<port>');
  await until(() => frame.contentDocument && frame.contentDocument.readyState === 'complete' && S());
  win().addEventListener('error', (e) => report.errors.push('error: ' + e.message));
  win().addEventListener('unhandledrejection', (e) => report.errors.push('rejection: ' + (e.reason && e.reason.message || e.reason)));
  const FRAME = 1e6 / 30;
  const near = (a, b, tolerance = 600) => Math.abs(a - b) <= tolerance;   // timestamps are whole milliseconds

  // 1. the clip from the command line plays by itself, frame after frame, at its own speed
  check(await until(() => S().cur && S().playing), 'the clip given on the command line starts playing');
  check($('clip-name').textContent === 'native_premul.webm' && $('drop-hint').hidden && /playable: yes/.test($('inspect').textContent),
    'its name and the inspect report are shown', $('clip-name').textContent);
  check(frame.contentDocument.title === 'AlphaClip preview' && $('btn-open').textContent === 'Choose files…', '?lang=en gives the English texts');
  const seen = [];
  const t0 = performance.now();
  const wall0 = S().cur.pts;
  while (performance.now() - t0 < 1200) {
    const p = S().cur.pts;
    if (seen[seen.length - 1] !== p) seen.push(p);
    await sleep(4);
  }
  const elapsed = (performance.now() - t0) * 1000;
  const steps = seen.slice(1).map((p, i) => p - seen[i]);
  check(seen.length >= 20 && steps.every((d) => near(d, FRAME, 1100)), 'playing shows every frame in order (no frame skipped)',
    JSON.stringify(steps));
  const advanced = seen[seen.length - 1] - wall0;
  check(advanced > 0.7 * elapsed && advanced < 1.3 * elapsed, 'playing runs at the clip\'s speed',
    Math.round(advanced / 1000) + ' ms of video in ' + Math.round(elapsed / 1000) + ' ms');

  // 2. pause, step, seek
  $('btn-play').click();
  const paused = S().cur.pts;
  await sleep(250);
  check(!S().playing && S().cur.pts === paused, 'pause stops on a frame');
  set('opt-loop', true);
  $('timeline').value = '300000';
  $('timeline').dispatchEvent(new (win().Event)('input', { bubbles: true }));
  check(await until(() => near(S().cur.pts, 9 * FRAME)), 'dragging the timeline to 0.300 s shows frame 9', S().cur.pts);
  let ok = true;
  for (let n = 10; n <= 12; n++) {
    await until(() => S().next);
    $('btn-next').click();
    ok = ok && await until(() => near(S().cur.pts, n * FRAME), 3000);
  }
  check(ok, 'three steps forward show frames 10, 11 and 12', S().cur.pts);
  $('btn-prev').click();
  check(await until(() => near(S().cur.pts, 11 * FRAME), 3000), 'one step back shows frame 11', S().cur.pts);
  check(/frame 11 \/ 60/.test($('time-label').textContent), 'the label names the frame', $('time-label').textContent);

  // 3. the end of the clip: stop without the loop, start again with it
  set('opt-loop', false);
  $('timeline').value = $('timeline').max;
  $('timeline').dispatchEvent(new (win().Event)('input', { bubbles: true }));
  check(await until(() => near(S().cur.pts, 59 * FRAME) && S().next === 'end'), 'the end of the timeline is the last frame', S().cur.pts);
  $('btn-play').click();   // at the end without the loop: play starts again from the beginning ...
  check(await until(() => S().playing && S().cur.pts < 10 * FRAME), 'play at the end starts from the beginning');
  check(await until(() => !S().playing && near(S().cur.pts, 59 * FRAME), 6000), '... and without the loop it stops on the last frame', S().cur.pts);
  set('opt-loop', true);
  $('btn-play').click();
  check(await until(() => S().playing && S().cur.pts < 10 * FRAME, 4000), 'with the loop it goes on from the first frame');
  $('btn-play').click();

  // 4. what is drawn: frame 20 at 100% over white, Gamma and Linear (the test compares the pixels)
  set('opt-zoom', '1');
  set('opt-filter', 'linear');
  set('opt-viewmode', '0');
  set('opt-blend', 'premultiplied');
  set('bg-checker', false);
  $('bg-presets').querySelector('button[data-color="#ffffff"]').click();
  $('timeline').value = '667000';
  $('timeline').dispatchEvent(new (win().Event)('input', { bubbles: true }));
  await until(() => near(S().cur.pts, 20 * FRAME));
  for (const space of ['gamma', 'linear']) {
    set('opt-colorspace', space);
    const scene = await sendPicture('ui.f20.' + space + '.white');
    const [x0, y0, x1, y1] = S().rect;
    const corner = Array.from(scene.data.subarray(0, 3));
    check(x1 - x0 === 320 && y1 - y0 === 180 && x0 > 0 && y0 > 0 && corner.every((c) => c === 255),
      '100%: the picture is 320x180 screen pixels in the middle, white around it [' + space + ']', [x0, y0, x1, y1, corner]);
  }
  set('opt-colorspace', 'gamma');
  set('opt-viewmode', '1');
  let scene = S().renderer.readScene();
  let o = (S().rect[1] * scene.width + S().rect[0]) * 4;
  check(scene.data[o] === 0 && scene.data[o + 1] === 0 && scene.data[o + 2] === 0, '"alpha only" shows the transparent corner black');
  set('opt-viewmode', '0');
  set('bg-checker', true);
  scene = S().renderer.readScene();
  const greys = new Set([scene.data[0], scene.data[4 * 8]]);
  check(greys.has(96) && greys.has(144), 'the checkerboard has its two greys', Array.from(greys));
  set('opt-zoom', '2');
  check(S().rect[2] - S().rect[0] === 640, '200% doubles the picture');
  $('view').dispatchEvent(new (win().WheelEvent)('wheel', { deltaY: -100, bubbles: true, cancelable: true }));
  check($('opt-zoom').value === '4', 'the wheel zooms in one step', $('opt-zoom').value);
  const rect0 = S().rect.slice();
  const P = win().PointerEvent;
  $('view').dispatchEvent(new P('pointerdown', { clientX: 100, clientY: 100, pointerId: 1, bubbles: true }));
  $('view').dispatchEvent(new P('pointermove', { clientX: 130, clientY: 90, pointerId: 1, bubbles: true }));
  $('view').dispatchEvent(new P('pointerup', { clientX: 130, clientY: 90, pointerId: 1, bubbles: true }));
  check(S().rect[0] - rect0[0] > 0 && S().rect[1] - rect0[1] < 0, 'dragging moves the picture', [rect0, S().rect]);
  set('opt-zoom', '1');
  const box = $('view').getBoundingClientRect();
  $('view').dispatchEvent(new P('pointermove', { clientX: box.left + box.width / 2, clientY: box.top + box.height / 2, pointerId: 1, bubbles: true }));
  check(/^x \d+ {2}y \d+ .* \d+ \d+ \d+ \d+$/.test($('readout').textContent), 'the pixel under the pointer is read out', $('readout').textContent);

  // 5. the sidecar editor: the core decides
  set('meta-alpha-mode', 'opaque');
  check(await until(() => S().clip.info.alpha_mode === 0 && S().cur) && $('meta-error').hidden && /"alpha_mode": "opaque"/.test($('meta-json').textContent),
    'alpha_mode changed to opaque: the clip is opened again as opaque');
  const out = S().renderer.readOutput();
  check(out.data[3] === 255 && out.data[out.data.length - 1] === 255, '... and its picture is opaque');
  set('meta-alpha-mode', 'packed_right');
  check(await until(() => !$('meta-error').hidden) && /AC_ERR_FORMAT/.test($('meta-error').textContent) && S().clip.info.alpha_mode === 0 && S().cur,
    'a sidecar the core refuses: its reason is shown and the picture stays', $('meta-error').textContent);
  set('meta-use', false);
  check(await until(() => S().clip.meta === null && S().clip.info.alpha_mode === 3) && $('meta-fields').disabled && $('meta-error').hidden,
    '"use a sidecar" off: the core\'s auto mode');
  $('meta-reset').click();
  check(await until(() => S().clip.meta && S().clip.info.premultiplied === 1 && S().clip.info.matrix === 1) && $('meta-premultiplied').checked,
    'Revert goes back to the sidecar the clip came with');

  // 6. files dropped on the page
  await drop(['packed_bottom_straight.webm', 'packed_bottom_straight.acmeta.json']);
  check(await until(() => S().clip.name === 'packed_bottom_straight.webm' && S().clip.info && S().clip.info.alpha_mode === 2 && S().cur) &&
    S().playing && $('meta-alpha-mode').value === 'packed_bottom' && !$('meta-premultiplied').checked && $('clip-name').textContent === 'packed_bottom_straight.webm',
    'a clip dropped with its sidecar is opened with it and plays');
  check(S().clip.info.width === 320 && S().clip.info.height === 360 && S().rect[3] - S().rect[1] === 180, 'a packed clip is shown at its picture\'s size');
  await drop(['opaque_override.json']);
  check(await until(() => S().clip.info.alpha_mode === 0) && S().clip.name === 'packed_bottom_straight.webm',
    'a sidecar dropped alone replaces the current clip\'s');
  await drop(['alpha_native.webm']);
  check(await until(() => S().clip.name === 'alpha_native.webm' && S().clip.info && S().clip.info.alpha_mode === 3 && S().cur) &&
    S().clip.meta === null && !$('meta-use').checked, 'a clip dropped without a sidecar opens in auto mode');
  await drop(['garbage.webm']);
  check(await until(() => S().clip.name === 'garbage.webm' && !$('meta-error').hidden) && S().clip.info === null && !$('drop-hint').hidden,
    'a file that is not a video: the reason is shown, nothing plays', $('meta-error').textContent);
  await drop(['native_premul.webm', 'native_premul.acmeta.json']);
  check(await until(() => S().clip.name === 'native_premul.webm' && S().clip.info && S().cur && S().playing) && $('meta-error').hidden,
    'after that a good clip plays again');
  $('btn-play').click();
}

run().catch((e) => {
  report.fatal = String(e && e.message ? e.message : e);
}).finally(async () => {
  document.getElementById('log').textContent += JSON.stringify(report.errors) + (report.fatal ? '\nfatal: ' + report.fatal : '') + '\n';
  await post('/done', JSON.stringify(report), 'application/json');
});
