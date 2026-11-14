// AlphaClip preview: the WebGL2 pipeline. It does what an engine binding does, in the same two steps:
//
//   1. "output":  Y/U/V[/A] planes -> premultiplied RGBA at the picture's size, with the master shader
//                 (shaders/alphaclip_yuv_to_rgba.frag.glsl, used as it is). This is AlphaClipPlayer.Output in Unity.
//   2. "scene":   a background, then the output drawn as a quad with blending One / OneMinusSrcAlpha, as a scene or
//                 a UI would. A third trivial step copies the scene to the canvas.
//
// Colour space. "Gamma" works on the encoded values in plain RGBA8 targets, like a Unity Gamma project. "Linear"
// uses real sRGB targets (SRGB8_ALPHA8): the shader outputs linear values, the hardware encodes them when writing,
// decodes them when sampling and blends in linear light, like a Unity Linear project.
//
// Rows: every texture and both targets keep the TOP row of the picture in row 0, so readOutput() / readScene() return
// rows top first. Only the final copy to the canvas flips (a canvas has its origin at the bottom).

export const MATRIX = {
  bt601: [1.402, 0.344136, 0.714136, 1.772],
  bt709: [1.5748, 0.187324, 0.468124, 1.8556],
};

// The sidecar's matrix and range win; what it leaves unspecified comes from the caller's defaults (in an engine:
// the component's own settings). `info` is the clip's ac_info as the service reports it.
export function effectiveMatrix(info, fallback) {
  return info.matrix === 2 ? 'bt709' : info.matrix === 1 ? 'bt601' : fallback;
}
export function effectiveFullRange(info, fallbackFull) {
  return info.color_range === 2 || (info.color_range !== 1 && !!fallbackFull);
}

const VS = `#version 300 es
in vec2 aPos;          // unit quad, 0..1
uniform vec4 uRect;    // where its corners (0,0) and (1,1) go, in clip space
out vec2 vUv;
void main() {
  vUv = aPos;
  gl_Position = vec4(mix(uRect.xy, uRect.zw, aPos), 0.0, 1.0);
}`;

const SRGB_GLSL = `
vec3 SrgbToLinear(vec3 c) {
  return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, step(c, vec3(0.04045)));
}
vec3 LinearToSrgb(vec3 c) {
  return mix(1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055, c * 12.92, step(c, vec3(0.0031308)));
}`;

// The background: one colour or a checkerboard. The colours are sRGB-encoded values, as a colour picker gives them.
const FS_BACKGROUND = `#version 300 es
precision highp float;
uniform vec3 uColorA;
uniform vec3 uColorB;
uniform float uCheckSize;   // 0: solid uColorA
uniform float uLinear;
out vec4 oColor;
${SRGB_GLSL}
void main() {
  vec3 c = uColorA;
  if (uCheckSize > 0.5) {
    vec2 p = floor(gl_FragCoord.xy / uCheckSize);
    c = mix(uColorA, uColorB, mod(p.x + p.y, 2.0));
  }
  if (uLinear > 0.5) c = SrgbToLinear(c);
  oColor = vec4(c, 1.0);
}`;

// The picture in the scene. uViewMode 0: the premultiplied picture, blended by the caller's blend state.
// 1: its alpha as a grey image. 2: its colour as the shader delivered it (premultiplied), over black.
const FS_PICTURE = `#version 300 es
precision highp float;
precision highp sampler2D;
uniform sampler2D uTex;
uniform float uViewMode;
uniform float uLinear;
in vec2 vUv;
out vec4 oColor;
${SRGB_GLSL}
void main() {
  vec4 c = texture(uTex, vUv);
  if (uViewMode > 1.5) {
    oColor = vec4(c.rgb, 1.0);
  } else if (uViewMode > 0.5) {
    vec3 g = vec3(c.a);
    if (uLinear > 0.5) g = SrgbToLinear(g);
    oColor = vec4(g, 1.0);
  } else {
    oColor = c;
  }
}`;

// Scene -> canvas, one texel per pixel. Sampling an sRGB texture yields linear values, and the canvas is not an
// sRGB target, so in Linear mode they are encoded again here (the round trip returns the stored bytes).
const FS_PRESENT = `#version 300 es
precision highp float;
precision highp sampler2D;
uniform sampler2D uTex;
uniform float uLinear;
in vec2 vUv;
out vec4 oColor;
${SRGB_GLSL}
void main() {
  vec3 c = texture(uTex, vec2(vUv.x, 1.0 - vUv.y)).rgb;
  if (uLinear > 0.5) c = LinearToSrgb(c);
  oColor = vec4(c, 1.0);
}`;

const FULL = [-1, -1, 1, 1];

export class Renderer {
  // `yuvFragmentSource` is the text of shaders/alphaclip_yuv_to_rgba.frag.glsl.
  constructor(canvas, yuvFragmentSource) {
    const gl = canvas.getContext('webgl2', {
      alpha: false, antialias: false, depth: false, stencil: false, premultipliedAlpha: false,
      preserveDrawingBuffer: false, powerPreference: 'high-performance',
    });
    if (!gl) throw new Error('WebGL2 is not available in this browser');
    this.canvas = canvas;
    this.gl = gl;
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.DITHER);

    this.progYuv = this._program(VS, yuvFragmentSource);
    this.progBackground = this._program(VS, FS_BACKGROUND);
    this.progPicture = this._program(VS, FS_PICTURE);
    this.progPresent = this._program(VS, FS_PRESENT);

    this.vao = gl.createVertexArray();
    gl.bindVertexArray(this.vao);
    const buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]), gl.STATIC_DRAW);
    // aPos is bound to location 0 in every program (see _program).
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);

    this.white = this._plane(1, 1, gl.NEAREST);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 1, 1, gl.RED, gl.UNSIGNED_BYTE, new Uint8Array([255]));

    this.clip = null;      // the clip's ac_info as the server reports it
    this.planes = null;    // {y, u, v, a}
    this.output = null;    // {tex, fbo, w, h, linear}
    this.scene = null;
  }

  // What the browser draws with (for the test report).
  describe() {
    const gl = this.gl;
    const ext = gl.getExtension('WEBGL_debug_renderer_info');
    return {
      version: gl.getParameter(gl.VERSION),
      shading: gl.getParameter(gl.SHADING_LANGUAGE_VERSION),
      vendor: ext ? gl.getParameter(ext.UNMASKED_VENDOR_WEBGL) : gl.getParameter(gl.VENDOR),
      renderer: ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER),
    };
  }

  // A new clip (or the same clip opened with other metadata): plane textures of the coded size.
  setClip(info) {
    const gl = this.gl;
    if (this.planes) {
      for (const t of Object.values(this.planes)) if (t) gl.deleteTexture(t);
    }
    const w = info.width, h = info.height, cw = (w + 1) >> 1, ch = (h + 1) >> 1;
    this.clip = info;
    this.planes = {
      y: this._plane(w, h, gl.NEAREST),
      u: this._plane(cw, ch, gl.LINEAR),
      v: this._plane(cw, ch, gl.LINEAR),
      a: info.has_alpha_plane ? this._plane(w, h, gl.NEAREST) : null,
    };
    this.hasFrame = false;
  }

  // Nothing to show any more (the clip was replaced): the scene is the background alone until the next frame.
  forget() {
    this.hasFrame = false;
  }

  // frame: {width, height, y, u, v, a} with tightly packed Uint8Array planes, rows top first.
  uploadFrame(frame) {
    const gl = this.gl, info = this.clip;
    if (!info || frame.width !== info.width || frame.height !== info.height) {
      throw new Error('the frame does not have the size of the clip');
    }
    const w = info.width, h = info.height, cw = (w + 1) >> 1, ch = (h + 1) >> 1;
    const put = (tex, pw, ph, data) => {
      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, pw, ph, gl.RED, gl.UNSIGNED_BYTE, data);
    };
    put(this.planes.y, w, h, frame.y);
    put(this.planes.u, cw, ch, frame.u);
    put(this.planes.v, cw, ch, frame.v);
    if (this.planes.a) {
      if (!frame.a) throw new Error('the server delivered no alpha plane for a native_alpha clip');
      put(this.planes.a, w, h, frame.a);
    }
    this.hasFrame = true;
  }

  // Step 1. p: {matrix: 'bt601'|'bt709', fullRange: bool, chromaShift: number (0.25 = left-sited, 0 = centred),
  // linear: bool}. The alpha mode and the premultiplied flag are the clip's (what the core read from the sidecar).
  drawOutput(p) {
    const gl = this.gl, info = this.clip;
    if (!info || !this.hasFrame) return false;
    const w = info.width, h = info.height, cw = (w + 1) >> 1, ch = (h + 1) >> 1;
    const pw = info.content_w, ph = info.content_h;
    const pcw = (pw + 1) >> 1, pch = (ph + 1) >> 1;
    this.output = this._target(this.output, pw, ph, p.linear);

    const full = !!p.fullRange;
    const prog = this.progYuv;
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.output.fbo);
    gl.viewport(0, 0, pw, ph);
    gl.disable(gl.BLEND);
    gl.useProgram(prog.program);
    this._bind(prog, 'uTexY', 0, this.planes.y);
    this._bind(prog, 'uTexU', 1, this.planes.u);
    this._bind(prog, 'uTexV', 2, this.planes.v);
    this._bind(prog, 'uTexA', 3, this.planes.a || this.white);
    gl.uniform4f(prog.u('uRect'), ...FULL);
    gl.uniform4f(prog.u('uYuvRange'), full ? 0 : 16 / 255, full ? 1 : 255 / 219, full ? 1 : 255 / 224, 0);
    gl.uniform4f(prog.u('uYuvCoeff'), ...MATRIX[p.matrix === 'bt709' ? 'bt709' : 'bt601']);
    gl.uniform1f(prog.u('uChromaShift'), p.chromaShift);
    gl.uniform2f(prog.u('uChromaTexel'), 1 / cw, 1 / ch);
    gl.uniform4f(prog.u('uContentRect'), pw / w, ph / h, info.alpha_x / w, info.alpha_y / h);
    gl.uniform4f(prog.u('uChromaClamp'), 0.5 / cw, 0.5 / ch, (pcw - 0.5) / cw, (pch - 0.5) / ch);
    gl.uniform1f(prog.u('uAlphaMode'), info.alpha_mode);
    gl.uniform1f(prog.u('uPremultiplied'), info.premultiplied ? 1 : 0);
    gl.uniform1f(prog.u('uLinearOutput'), p.linear ? 1 : 0);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    return true;
  }

  // Step 2. v: {width, height: size of the scene in pixels,
  //             rect: [x0, y0, x1, y1] where the picture goes, in scene pixels from the top-left corner,
  //             filter: 'linear'|'nearest', viewMode: 0|1|2, blend: 'premultiplied'|'straight',
  //             background: {color: [r,g,b] 0..255, checker: null | {color: [r,g,b], size: pixels}}, linear: bool}
  // `linear` must be what drawOutput was called with.
  drawScene(v) {
    const gl = this.gl;
    this.scene = this._target(this.scene, v.width, v.height, v.linear);
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.scene.fbo);
    gl.viewport(0, 0, v.width, v.height);
    gl.disable(gl.BLEND);

    const bg = this.progBackground;
    gl.useProgram(bg.program);
    gl.uniform4f(bg.u('uRect'), ...FULL);
    gl.uniform3f(bg.u('uColorA'), ...v.background.color.map((c) => c / 255));
    const checker = v.background.checker;
    gl.uniform3f(bg.u('uColorB'), ...(checker ? checker.color : v.background.color).map((c) => c / 255));
    gl.uniform1f(bg.u('uCheckSize'), checker ? checker.size : 0);
    gl.uniform1f(bg.u('uLinear'), v.linear ? 1 : 0);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

    if (!this.output || !this.hasFrame) return;
    const pic = this.progPicture;
    gl.useProgram(pic.program);
    this._bind(pic, 'uTex', 0, this.output.tex);
    const filter = v.filter === 'nearest' ? gl.NEAREST : gl.LINEAR;
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
    const [x0, y0, x1, y1] = v.rect;
    gl.uniform4f(pic.u('uRect'), x0 / v.width * 2 - 1, y0 / v.height * 2 - 1, x1 / v.width * 2 - 1, y1 / v.height * 2 - 1);
    gl.uniform1f(pic.u('uViewMode'), v.viewMode || 0);
    gl.uniform1f(pic.u('uLinear'), v.linear ? 1 : 0);
    gl.enable(gl.BLEND);
    gl.blendEquation(gl.FUNC_ADD);
    // Premultiplied: the colour already holds colour * alpha. "straight" is the wrong way on purpose (a control):
    // it multiplies by alpha a second time and darkens every soft edge.
    gl.blendFunc(v.blend === 'straight' ? gl.SRC_ALPHA : gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.disable(gl.BLEND);
  }

  // Step 3: the scene, one texel per canvas pixel.
  present() {
    const gl = this.gl;
    if (!this.scene) return;
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
    gl.disable(gl.BLEND);
    const prog = this.progPresent;
    gl.useProgram(prog.program);
    this._bind(prog, 'uTex', 0, this.scene.tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.uniform4f(prog.u('uRect'), ...FULL);
    gl.uniform1f(prog.u('uLinear'), this.scene.linear ? 1 : 0);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
  }

  // The stored bytes of a target as RGBA, rows top first (in Linear mode: the sRGB-encoded bytes).
  readOutput() { return this._read(this.output); }
  readScene() { return this._read(this.scene); }

  _read(target) {
    const gl = this.gl;
    if (!target) return null;
    const data = new Uint8Array(target.w * target.h * 4);
    gl.bindFramebuffer(gl.FRAMEBUFFER, target.fbo);
    gl.readPixels(0, 0, target.w, target.h, gl.RGBA, gl.UNSIGNED_BYTE, data);
    return { width: target.w, height: target.h, data };
  }

  _plane(w, h, filter) {
    const gl = this.gl;
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.R8, w, h);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return tex;
  }

  // A colour target of the given size and encoding; `old` is reused when it already fits.
  _target(old, w, h, linear) {
    const gl = this.gl;
    if (old && old.w === w && old.h === h && old.linear === !!linear) return old;
    if (old) {
      gl.deleteFramebuffer(old.fbo);
      gl.deleteTexture(old.tex);
    }
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texStorage2D(gl.TEXTURE_2D, 1, linear ? gl.SRGB8_ALPHA8 : gl.RGBA8, w, h);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    const fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
    if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) {
      throw new Error('cannot create a ' + w + 'x' + h + ' render target');
    }
    return { tex, fbo, w, h, linear: !!linear };
  }

  _bind(prog, name, unit, tex) {
    const gl = this.gl;
    gl.activeTexture(gl.TEXTURE0 + unit);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.uniform1i(prog.u(name), unit);
  }

  _program(vsSource, fsSource) {
    const gl = this.gl;
    const compile = (type, source) => {
      const s = gl.createShader(type);
      gl.shaderSource(s, source);
      gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
        throw new Error('shader: ' + gl.getShaderInfoLog(s));
      }
      return s;
    };
    const program = gl.createProgram();
    gl.attachShader(program, compile(gl.VERTEX_SHADER, vsSource));
    gl.attachShader(program, compile(gl.FRAGMENT_SHADER, fsSource));
    gl.bindAttribLocation(program, 0, 'aPos');
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      throw new Error('shader link: ' + gl.getProgramInfoLog(program));
    }
    const cache = new Map();
    return {
      program,
      u: (name) => {
        if (!cache.has(name)) cache.set(name, gl.getUniformLocation(program, name));
        return cache.get(name);
      },
    };
  }
}
