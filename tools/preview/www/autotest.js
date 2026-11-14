// AlphaClip preview: the test driver (core/tests/test_preview.py opens autotest.html?sink=http://127.0.0.1:<port>).
// It runs the same renderer.js and api.js as the page on a list of jobs and posts back what was drawn, so the
// test can compare the bytes with a CPU reference, with ground truth and with what Unity drew.
//
// The "sink" is a small collector the test runs on this machine:
//   GET  <sink>/jobs            {"jobs": [job, ...]}
//   GET  <sink>/file/<name>     the bytes of a clip
//   POST <sink>/result?name=N&width=W&height=H&pts=P   body: RGBA bytes, rows top first
//   POST <sink>/done            {"gl": {...}, "userAgent", "errors": [...], "fatal": null | text}
// job: {name, clip, meta: sidecar text | null, at_us, linear, default_matrix: 'bt601'|'bt709',
//       default_full_range, chroma_shift, want_output,
//       scene: null | {bg: [r,g,b], blend: 'premultiplied'|'straight', zoom, filter: 'linear'|'nearest', view_mode}}
import { Renderer, effectiveMatrix, effectiveFullRange } from './renderer.js';
import * as api from './api.js';

const sink = new URLSearchParams(location.search).get('sink') || '';
const log = (text) => {
  document.getElementById('log').textContent += text + '\n';
};

async function post(path, body, type) {
  const res = await fetch(sink + path, { method: 'POST', headers: { 'Content-Type': type }, body });
  if (!res.ok) throw new Error('the sink answered ' + res.status + ' to ' + path);
}

async function sendPixels(name, pixels, pts) {
  await post('/result?' + new URLSearchParams({ name, width: pixels.width, height: pixels.height, pts }),
    pixels.data, 'application/octet-stream');
}

async function run() {
  // Results only ever go to a collector on this machine.
  if (!/^http:\/\/127\.0\.0\.1:\d{1,5}$/.test(sink)) throw new Error('give ?sink=http://127.0.0.1:<port>');
  const report = { gl: null, userAgent: navigator.userAgent, errors: [], fatal: null, jobs: 0 };
  try {
    const shader = await fetch('shaders/alphaclip_yuv_to_rgba.frag.glsl');
    if (!shader.ok) throw new Error('cannot load the shader');
    const renderer = new Renderer(document.getElementById('view'), await shader.text());
    report.gl = renderer.describe();
    const plan = await (await fetch(sink + '/jobs')).json();

    let clipName = null, clipId = 0, info = null, openedWith;
    for (const job of plan.jobs) {
      try {
        if (job.clip !== clipName) {
          const bytes = await (await fetch(sink + '/file/' + encodeURIComponent(job.clip))).arrayBuffer();
          clipId = (await api.uploadClip(job.clip, bytes)).id;
          clipName = job.clip;
          openedWith = undefined;
        }
        const meta = job.meta || null;
        if (meta !== openedWith) {
          info = (await api.openClip(clipId, meta)).info;
          renderer.setClip(info);
          openedWith = meta;
        }
        const frame = await api.fetchFrame(clipId, { at_us: job.at_us });
        if (!frame) throw new Error('no frame at ' + job.at_us);
        renderer.uploadFrame(frame);
        renderer.drawOutput({
          matrix: effectiveMatrix(info, job.default_matrix),
          fullRange: effectiveFullRange(info, job.default_full_range),
          chromaShift: job.chroma_shift, linear: !!job.linear,
        });
        if (job.want_output) await sendPixels(job.name + '.output', renderer.readOutput(), frame.pts);
        if (job.scene) {
          const zoom = job.scene.zoom || 1;
          const w = info.content_w * zoom, h = info.content_h * zoom;
          renderer.drawScene({
            width: w, height: h, rect: [0, 0, w, h], linear: !!job.linear,
            filter: job.scene.filter || 'linear', viewMode: job.scene.view_mode || 0,
            blend: job.scene.blend || 'premultiplied',
            background: { color: job.scene.bg, checker: null },
          });
          await sendPixels(job.name + '.scene', renderer.readScene(), frame.pts);
          renderer.present();
        }
        report.jobs++;
      } catch (e) {
        report.errors.push({ job: job.name, error: String(e && e.message ? e.message : e) });
      }
    }
  } catch (e) {
    report.fatal = String(e && e.message ? e.message : e);
  }
  log(JSON.stringify(report, null, 2));
  await post('/done', JSON.stringify(report), 'application/json');
}

run().catch((e) => log('fatal: ' + e));
