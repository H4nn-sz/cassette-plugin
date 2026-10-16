// Genera index.html (composición HyperFrames) con las formas de onda reales de antes/despues.
// Uso: node build.mjs
import { execFileSync } from 'node:child_process';
import { writeFileSync } from 'node:fs';

const W = 1500, H = 330;                     // plano ANTES
const W2 = 840, H2 = 210;                    // plano DESPUÉS (osciloscopio arriba a la derecha)

function envelope(file, cols) {
  const buf = execFileSync('ffmpeg', ['-v', 'error', '-i', file, '-ac', '1', '-ar', '8000', '-f', 'f32le', '-'],
                           { maxBuffer: 1 << 28 });
  const s = new Float32Array(buf.buffer, buf.byteOffset, buf.byteLength / 4);
  const per = Math.floor(s.length / cols), out = [];
  for (let i = 0; i < cols; i++) {
    let a = 0; for (let j = i * per; j < (i + 1) * per; j++) a += s[j] * s[j];
    out.push(Math.sqrt(a / per));
  }
  const sorted = [...out].sort((a, b) => a - b), ref = sorted[Math.floor(sorted.length * 0.97)] || 1;
  const n = out.map(v => Math.min(1, v / ref));
  return n.map((v, i) => (v * 2 + (n[i - 1] ?? v) + (n[i + 1] ?? v)) / 4);
}
const rng = seed => () => { seed |= 0; seed = seed + 0x6D2B79F5 | 0; let t = Math.imul(seed ^ seed >>> 15, 1 | seed);
  t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; };
const gauss = r => (r() + r() + r() + r() - 2) / 2;

function barsPath(env, W, H) {
  const bw = 8, gap = 4, n = Math.floor(W / (bw + gap)); let d = '';
  for (let i = 0; i < n; i++) {
    const v = env[Math.min(env.length - 1, Math.floor(i / n * env.length))];
    const h = Math.max(6, Math.pow(v, 0.85) * (H - 10));
    d += `M${i * (bw + gap)} ${(H / 2 - h / 2).toFixed(1)}h${bw}v${h.toFixed(1)}h-${bw}z`;
  }
  return d;
}
function dotsPath(env, W, H) {
  const r = rng(1994), step = 3, n = Math.floor(W / step); let d = '';
  for (let i = 0; i < n; i++) {
    const v = env[Math.min(env.length - 1, Math.floor(i / n * env.length))];
    const cnt = 10 + Math.round(v * 70), x = i * step;
    for (let k = 0; k < cnt; k++) {
      const y = H / 2 + gauss(r) * (6 + v * (H - 20)) * 0.55;
      d += `M${x} ${y.toFixed(1)}h2.4v2.4h-2.4z`;
    }
    if (v > 0.15) for (let k = 0; k < 3; k++) if (r() < v * 0.55) d += `M${x} ${(H / 2 + (r() * 2 - 1) * (H / 2 - 4) * Math.min(1, v * 1.1)).toFixed(1)}h2.6v2.6h-2.6z`;
  }
  return d;
}
const before = barsPath(envelope('assets/antes.mp3', 200), W, H);
const after = dotsPath(envelope('assets/despues.mp3', 300), W2, H2);

// Marcadores sobre los knobs reales de la captura (coordenadas CSS de la UI 1000×700)
const TAPE = [   // [n, x, y, título, descripción]
  [1, 547, 452, 'EDAD', 'Un solo knob: wow, hiss, head bump y dropouts'],
  [2, 556, 533, 'WOBBLE', 'Wow & flutter estéreo, la cinta que se estira'],
  [3, 648, 630, 'CRACKLE', 'Polvo y chasquidos de vinilo'],
  [4, 741, 630, 'STEREO', 'Ancho de 0% (mono) a 200%'],
];
const SPACE = [
  [1, 548, 452, 'REVERB', 'Cuánto del lugar se escucha'],
  [2, 569, 533, 'TAMAÑO', 'Qué tan grande es y cuánto dura la cola'],
  [3, 915, 533, 'ANCHO', 'Qué tan abierto suena en estéreo'],
  [4, 200, 90, 'ROOM · HALL · SPACE', 'Tres espacios, con eco de cinta en Space'],
];
const marker = (m, face) => `<div class="mk ${face}" id="mk${face}${m[0]}" style="left:${m[1] - 40}px;top:${m[2] - 40}px"><i></i><b>${m[0]}</b></div>`;
const legend = (rows, face) => rows.map(m => `<div class="lg ${face}" id="lg${face}${m[0]}" style="top:${(m[0] - 1) * 128}px"><span>${m[0]}</span><div><h3>${m[3]}</h3><p>${m[4]}</p></div></div>`).join('\n');

const html = `<!doctype html>
<html lang="es">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=1920, height=1080" />
<script src="https://cdn.jsdelivr.net/npm/gsap@3.14.2/dist/gsap.min.js"></script>
<style>
*{margin:0;padding:0;box-sizing:border-box}
html,body{width:1920px;height:1080px;overflow:hidden;background:#0c0c0c}
:root{--mono:"Cascadia Mono","Consolas","SF Mono",ui-monospace,monospace;--red:#e2382e}
#root{position:relative;width:1920px;height:1080px;font-family:var(--mono);overflow:hidden}
.scene{position:absolute;inset:0;perspective:2400px;perspective-origin:50% 42%;transform-style:flat}
.dark{background:#0c0c0c;color:#f2f2f0}
.light{background:#dcdcd9;color:#0c0c0c}
.gray{background:#8d8d8a;color:#3a3a39;filter:grayscale(1) contrast(.82) brightness(.92)}
.abs{position:absolute}
.p3d{transform-style:preserve-3d}
.h1{font-weight:800;letter-spacing:-6px;line-height:.9}
.tag{font-weight:700;letter-spacing:6px;font-size:30px}
.sub{font-weight:600;font-size:34px;letter-spacing:1px;opacity:.75}
.card{position:absolute;border-radius:34px;box-shadow:0 60px 120px rgba(0,0,0,.55),0 0 0 2px rgba(255,255,255,.06)}
.card.l{box-shadow:0 60px 110px rgba(0,0,0,.35)}
.plane{position:absolute}
.plane svg{position:absolute;inset:0;width:100%;height:100%}
.dot{display:inline-block;width:26px;height:26px;border-radius:50%;background:var(--red);margin-right:20px}
.foot{position:absolute;left:0;right:0;bottom:40px;text-align:center;font-weight:600;font-size:22px;letter-spacing:3px;opacity:.55}
.cta{font-weight:800;font-size:54px;letter-spacing:1px;padding:30px 60px;border-radius:80px;background:var(--red);color:#fff;display:inline-block}
.face{position:absolute;left:0;top:0;width:1000px;height:700px;backface-visibility:hidden;transform-style:preserve-3d}
.face.back{transform:rotateY(180deg)}
.face img{position:absolute;left:0;top:0;width:1000px;height:700px;border-radius:34px;box-shadow:0 60px 120px rgba(0,0,0,.6),0 0 0 2px rgba(255,255,255,.07)}
.mk{position:absolute;width:80px;height:80px;transform:translateZ(70px);backface-visibility:hidden}
.mk i{position:absolute;inset:0;border:5px solid var(--red);border-radius:50%;box-shadow:0 0 30px rgba(226,56,46,.8)}
.mk b{position:absolute;right:-18px;top:-22px;width:46px;height:46px;border-radius:50%;background:var(--red);color:#fff;font-size:28px;font-weight:800;display:grid;place-items:center}
.lg{position:absolute;left:0;width:760px;height:112px;display:flex;align-items:center;gap:28px}
.lg span{flex:none;width:68px;height:68px;border-radius:50%;background:var(--red);color:#fff;font-size:38px;font-weight:800;display:grid;place-items:center}
.lg h3{font-size:46px;font-weight:800;letter-spacing:1px}
.lg p{font-size:27px;font-weight:600;opacity:.7;margin-top:6px}
.vig{position:absolute;inset:0;background:radial-gradient(ellipse at 50% 48%,rgba(0,0,0,0) 38%,rgba(0,0,0,.5) 100%);pointer-events:none}
.glow{position:absolute;left:-200px;top:200px;width:1300px;height:1000px;background:radial-gradient(closest-side,rgba(226,56,46,.5),rgba(226,56,46,0));pointer-events:none}
.flash{position:absolute;inset:0;background:#fff;pointer-events:none}
.ring{position:absolute;left:810px;top:390px;width:300px;height:300px;border:10px solid var(--red);border-radius:50%;pointer-events:none}
</style>
</head>
<body>
<div id="root" data-composition-id="main" data-start="0" data-duration="21.5" data-width="1920" data-height="1080">

  <!-- AUDIO continuo: música + antes (seco, más bajo) + tu FX de transición, mezclado en una sola pista -->
  <audio id="aMusic" data-start="0" data-duration="21.5" data-track-index="20" src="assets/music_fx.mp3"></audio>

  <!-- 1 · INTRO (2 s) -->
  <section id="s1" class="clip scene dark" data-start="0" data-duration="2.1" data-track-index="1">
    <div id="s1card" class="abs p3d" style="left:1000px;top:240px;width:880px;height:616px">
      <img class="card" src="assets/plugin_c.png" style="left:0;top:0;width:880px;height:616px" />
    </div>
    <div id="s1txt" class="abs" style="left:110px;top:330px">
      <div class="h1" id="s1h" style="font-size:165px">CASSETTE</div>
      <div class="sub" id="s1sub" style="margin-top:34px">lo-fi tape &amp; space · VST3</div>
    </div>
  </section>

  <!-- 2 · ANTES (gris, apagado, con foco) -->
  <section id="s2" class="clip scene gray" data-start="1.8" data-duration="5.8" data-track-index="2">
    <div id="s2wrap" class="abs" style="inset:0">
      <div class="abs" style="left:110px;top:90px">
        <div class="tag" id="s2tag" style="color:#555553">SIN PLUGIN</div>
        <div class="h1" id="s2h" style="font-size:250px;margin-top:20px;color:#4a4a49">ANTES</div>
      </div>
      <div id="s2plane" class="abs p3d" style="left:210px;top:470px;width:${W}px;height:${H}px">
        <div class="plane" style="left:0;top:0;width:${W}px;height:${H}px">
          <svg viewBox="0 0 ${W} ${H}"><path d="${before}" fill="#767674"/></svg>
          <div id="s2played" class="plane" style="left:0;top:0;width:${W}px;height:${H}px;clip-path:inset(0 100% 0 0)">
            <svg viewBox="0 0 ${W} ${H}"><path d="${before}" fill="#3b3b3a"/></svg>
          </div>
          <div id="s2head" class="abs" style="left:0;top:-14px;width:5px;height:${H + 28}px;background:#e6e6e3"></div>
        </div>
      </div>
      <div class="foot" style="color:#3a3a39">PLANO · SIN CARÁCTER</div>
    </div>
    <div class="vig"></div>
  </section>

  <!-- 3 · DESPUÉS (tema oscuro, dinámico) -->
  <section id="s3" class="clip scene dark" data-start="7.5" data-duration="9.0" data-track-index="3">
    <div id="s3glow" class="glow"></div>
    <div id="s3shake" class="abs" style="inset:0">
      <div class="abs" style="left:110px;top:60px;z-index:5">
        <div class="tag" id="s3tag" style="color:var(--red)"><span class="dot"></span>CON CASSETTE</div>
        <div class="h1" id="s3h" style="font-size:190px;margin-top:20px">DESPUÉS</div>
      </div>

      <div id="s3plane" class="abs p3d" style="left:1010px;top:80px;width:${W2}px;height:${H2}px">
        <div class="plane" style="left:0;top:0;width:${W2}px;height:${H2}px">
          <svg viewBox="0 0 ${W2} ${H2}"><path d="${after}" fill="#4a4a47"/></svg>
          <div id="s3played" class="plane" style="left:0;top:0;width:${W2}px;height:${H2}px;clip-path:inset(0 100% 0 0)">
            <svg viewBox="0 0 ${W2} ${H2}"><path d="${after}" fill="#f2f2f0"/></svg>
          </div>
          <div id="s3head" class="abs" style="left:0;top:-10px;width:4px;height:${H2 + 20}px;background:var(--red)"></div>
        </div>
      </div>

      <div class="abs p3d" style="left:80px;top:330px;width:900px;height:630px;transform:scale(.9);transform-origin:0 0">
        <div id="s3beat" class="abs p3d" style="left:0;top:0;width:1000px;height:700px;transform-origin:50% 50%">
          <div id="s3card" class="abs p3d" style="left:0;top:0;width:1000px;height:700px">
            <div id="s3flip" class="abs p3d" style="left:0;top:0;width:1000px;height:700px">
              <div class="face front">
                <img src="assets/plugin_c.png" />
                ${TAPE.map(m => marker(m, 'T')).join('\n                ')}
              </div>
              <div class="face back">
                <img src="assets/plugin_b.png" />
                ${SPACE.map(m => marker(m, 'S')).join('\n                ')}
              </div>
            </div>
          </div>
        </div>
      </div>

      <div class="abs" style="left:1090px;top:380px;width:780px;height:540px">
        ${legend(TAPE, 'T')}
        ${legend(SPACE, 'S')}
      </div>
    </div>
    <div id="s3ring" class="ring"></div>
    <div id="s3flash" class="flash"></div>
  </section>

  <!-- 4 · CIERRE -->
  <section id="s4" class="clip scene light" data-start="16.2" data-duration="5.3" data-track-index="4">
    <div id="s4card" class="abs p3d" style="left:1010px;top:250px;width:860px;height:602px">
      <img class="card l" src="assets/plugin_a.png" style="left:0;top:0;width:860px;height:602px" />
    </div>
    <div class="abs" style="left:110px;top:210px">
      <div class="tag" id="s4tag" style="color:var(--red)">VST3 · WINDOWS</div>
      <div class="h1" id="s4h" style="font-size:155px;margin-top:26px">CASSETTE</div>
      <div class="sub" id="s4sub" style="margin-top:30px;color:#0c0c0c;max-width:820px;line-height:1.35">Wow &amp; flutter · saturación · crackle · stereo · reverb</div>
      <div id="s4cta" style="margin-top:64px"><span class="cta">Comenta “EFECTO”</span></div>
      <div class="sub" id="s4cta2" style="margin-top:26px;font-size:40px;font-weight:800;opacity:1">y te lo paso</div>
    </div>
  </section>
</div>

<script>
window.__timelines = window.__timelines || {};
const tl = gsap.timeline({ paused: true });
const E = 'power3.out';
const out = (id, t, d = .6) => tl.to(id, { rotateY: -38, x: -260, scale: .9, duration: d, ease: 'power2.in' }, t);
const into = (id, t) => tl.from(id, { rotateY: 38, x: 260, scale: .9, duration: .7, ease: E }, t);

// ── 1 · INTRO (0–2): rápido
tl.from('#s1card', { rotateY: -70, rotateX: 18, x: 420, z: -500, duration: .9, ease: 'power4.out' }, 0.02)
  .to('#s1card', { rotateY: -14, rotateX: 6, y: -24, duration: 1.8, ease: 'sine.inOut' }, .6)
  .from('#s1h', { x: -160, opacity: 0, duration: .6, ease: E }, .05)
  .from('#s1sub', { y: 40, opacity: 0, duration: .5, ease: E }, .35);
out('#s1', 1.55, .5);

// ── 2 · ANTES (1.8–7.6): gris, apagado, con foco lento. Audio seco y bajo
into('#s2', 1.8);
tl.fromTo('#s2wrap', { filter: 'blur(16px)' }, { filter: 'blur(0px)', duration: 1.0, ease: 'power2.out' }, 1.9)
  .to('#s2wrap', { scale: 1.07, duration: 5.4, ease: 'none' }, 2.0)
  .from('#s2h', { x: -120, opacity: 0, duration: .9, ease: E }, 2.1)
  .from('#s2tag', { opacity: 0, duration: .8, ease: E }, 2.3)
  .fromTo('#s2plane', { rotateX: 58, rotateY: 14, y: 60, opacity: 0 },
                      { rotateX: 54, rotateY: 4, y: 0, opacity: 1, duration: 1.0, ease: E }, 2.0)
  .fromTo('#s2played', { clipPath: 'inset(0 100% 0 0)' }, { clipPath: 'inset(0 0% 0 0)', duration: 5.5, ease: 'none' }, 2.0)
  .fromTo('#s2head', { x: 0 }, { x: ${W - 5}, duration: 5.5, ease: 'none' }, 2.0);
// el audio se ahoga justo antes del golpe (7.2 → 7.5) y cae el brillo de la escena
tl.to('#s2wrap', { opacity: .35, duration: .3, ease: 'power1.in' }, 7.2);

// ── 3 · DESPUÉS (7.5–16.5): corte seco + flash + onda expansiva + tu FX
tl.fromTo('#s3flash', { opacity: 1 }, { opacity: 0, duration: .5, ease: 'power2.out' }, 7.5)
  .fromTo('#s3ring', { scale: .1, opacity: .9 }, { scale: 7, opacity: 0, duration: .9, ease: 'power2.out' }, 7.5)
  .fromTo('#s3shake', { x: 0, y: 0 }, { keyframes: { x: [0, -26, 22, -14, 9, -4, 0], y: [0, 14, -18, 10, -6, 3, 0] }, duration: .55, ease: 'none' }, 7.5)
  .fromTo('#s3glow', { opacity: 0 }, { opacity: 1, duration: .5, ease: E }, 7.5)
  .from('#s3h', { scale: 2.6, x: 200, opacity: 0, rotate: -5, duration: .55, ease: 'expo.out', transformOrigin: '0 50%' }, 7.5)
  .from('#s3tag', { y: -40, opacity: 0, duration: .4, ease: E }, 7.7)
  .from('#s3card', { rotateY: 200, x: -600, z: -600, duration: .9, ease: 'power4.out' }, 7.5)
  .to('#s3card', { rotateY: 12, rotateX: 4, duration: 4.4, ease: 'sine.inOut' }, 8.5)
  .to('#s3card', { rotateY: -8, rotateX: 3, duration: 3.4, ease: 'sine.inOut' }, 12.9)
  .fromTo('#s3plane', { rotateX: 60, rotateY: -50, y: -120, opacity: 0, scale: 1.4 },
                      { rotateX: 22, rotateY: -14, y: 0, opacity: 1, scale: 1, duration: .8, ease: 'back.out(1.5)' }, 7.6)
  .fromTo('#s3played', { clipPath: 'inset(0 100% 0 0)' }, { clipPath: 'inset(0 0% 0 0)', duration: 9, ease: 'none' }, 7.5)
  .fromTo('#s3head', { x: 0 }, { x: ${W2 - 4}, duration: 9, ease: 'none' }, 7.5);

// pulso a ritmo (~101 BPM): la tarjeta y el resplandor laten con cada tiempo
for (let n = 14, t = 0.34 + 14 * 0.594; t < 16.2; n++, t = 0.34 + n * 0.594) {
  tl.fromTo('#s3beat', { scale: 1 }, { scale: 1.035, duration: .1, ease: 'power2.out', yoyo: true, repeat: 1 }, t)
    .fromTo('#s3glow', { opacity: .75 }, { opacity: 1, duration: .22, ease: 'power2.out' }, t);
}

const T = [8.7, 9.5, 10.3, 11.1], S = [13.0, 13.8, 14.6, 15.3];
const mk = (pre, n, t) => tl.from('#mk' + pre + n, { scale: 0, z: -300, opacity: 0, duration: .4, ease: 'back.out(2.4)' }, t)
  .to('#mk' + pre + n + ' i', { scale: 1.25, duration: .35, ease: 'sine.inOut', yoyo: true, repeat: 1 }, t + .35)
  .from('#lg' + pre + n, { x: 160, opacity: 0, duration: .4, ease: 'expo.out' }, t);
T.forEach((t, i) => mk('T', i + 1, t));
tl.to(['#lgT1', '#lgT2', '#lgT3', '#lgT4'], { x: -80, opacity: 0, duration: .3, ease: 'power2.in', stagger: .04 }, 11.85)
  .to('#s3flip', { rotateY: 180, duration: .8, ease: 'power3.inOut' }, 12.0)
  .fromTo('#s3beat', { scale: 1 }, { scale: 1.08, duration: .4, ease: 'power2.out', yoyo: true, repeat: 1 }, 12.1);
S.forEach((t, i) => mk('S', i + 1, t));
out('#s3', 15.95, .55);

// ── 4 · CIERRE (16.2–21.5)
into('#s4', 16.2);
tl.from('#s4card', { rotateY: -80, x: 400, z: -400, duration: 1.0, ease: 'power4.out' }, 16.4)
  .to('#s4card', { rotateY: -12, rotateX: 5, y: -20, duration: 4, ease: 'sine.inOut' }, 17.4)
  .from('#s4tag', { y: -30, opacity: 0, duration: .5, ease: E }, 16.6)
  .from('#s4h', { x: -180, opacity: 0, duration: .7, ease: E }, 16.7)
  .from('#s4sub', { y: 40, opacity: 0, duration: .5, ease: E }, 17.2)
  .from('#s4cta', { y: 60, opacity: 0, scale: .85, duration: .6, ease: 'back.out(1.7)' }, 17.8)
  .from('#s4cta2', { y: 30, opacity: 0, duration: .5, ease: E }, 18.3)
  .to('#s4cta', { scale: 1.05, duration: .45, ease: 'sine.inOut', yoyo: true, repeat: 5 }, 18.8);

window.__timelines['main'] = tl;
tl.seek(0);
</script>
</body>
</html>`;

writeFileSync('index.html', html);
console.log('index.html', (html.length / 1024).toFixed(0) + ' KB');
