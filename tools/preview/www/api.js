// AlphaClip preview: the calls to the local service (`alphaclip preview`). The service owns the clip and decodes it
// with the native core; the page only gets YUV(A) planes.

async function jsonOrThrow(res) {
  let body = null;
  try {
    body = await res.json();
  } catch (e) {
    body = null;
  }
  if (!res.ok) {
    const err = new Error(body && body.error ? body.error : 'HTTP ' + res.status);
    err.status = body && body.status ? body.status : '';   // the core's status name, e.g. AC_ERR_FORMAT
    err.http = res.status;
    throw err;
  }
  return body;
}

// {version, clip: null | {id, name, size, meta: string|null, info: {...}|null, inspect: string, error: string|null}}
export async function getState() {
  return jsonOrThrow(await fetch('/api/state'));
}

// Hands the bytes of a .webm to the service. Returns {id, name, size}; the clip is not opened yet.
export async function uploadClip(name, bytes) {
  return jsonOrThrow(await fetch('/api/clips?name=' + encodeURIComponent(name), {
    method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: bytes,
  }));
}

// Opens (or re-opens) a clip with the given sidecar text; null or '' = no sidecar ("auto"). Returns
// {id, name, size, meta, info, inspect}. When the core refuses, throws (err.status = AC_ERR_...) and the clip stays
// as it was.
export async function openClip(id, metaText) {
  return jsonOrThrow(await fetch('/api/clips/' + id + '/open', {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: metaText || '',
  }));
}

// query: {at_us: T} the frame shown at time T, or {after_us: P} the frame that follows the one with pts P.
// Returns a frame, or null when there is none after P (end of the clip).
export async function fetchFrame(id, query) {
  const res = await fetch('/api/clips/' + id + '/frame?' + new URLSearchParams(query));
  if (res.status === 204) return null;
  if (!res.ok) return jsonOrThrow(res);
  return parseFrame(await res.arrayBuffer());
}

// 32-byte little-endian header ("ACF1", width, height, flags (bit 0: alpha plane), pts_us as int64, 8 reserved
// bytes), then the Y, U, V and (with the flag) A planes, tightly packed, rows top first.
export function parseFrame(buffer) {
  const view = new DataView(buffer);
  if (buffer.byteLength < 32 || view.getUint32(0, false) !== 0x41434631) throw new Error('not a frame');
  const width = view.getUint32(4, true), height = view.getUint32(8, true);
  const hasAlpha = (view.getUint32(12, true) & 1) !== 0;
  const pts = Number(view.getBigInt64(16, true));
  const cw = (width + 1) >> 1, ch = (height + 1) >> 1;
  const ySize = width * height, cSize = cw * ch;
  if (buffer.byteLength !== 32 + ySize + 2 * cSize + (hasAlpha ? ySize : 0)) throw new Error('frame of the wrong size');
  let at = 32;
  const take = (n) => {
    const plane = new Uint8Array(buffer, at, n);
    at += n;
    return plane;
  };
  return { width, height, pts, y: take(ySize), u: take(cSize), v: take(cSize), a: hasAlpha ? take(ySize) : null };
}
