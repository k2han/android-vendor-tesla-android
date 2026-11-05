// mic-capture.js: Tesla cabin microphone -> Tesla Android (wss://<host>/sockets/mic).
// Starts on the first tap (browser needs a user gesture), sends PCM s16le mono 48 kHz
// in 20 ms frames. Fully optional: if the API or permission is missing, nothing else changes.
(function () {
  "use strict";
  if (window.__taMic) return;
  const state = { ws: null, ctx: null, stream: null, node: null, started: false, status: "off" };
  window.__taMic = state;
  const RATE = 48000, FRAME = 960;

  const dot = document.createElement("div");
  dot.style.cssText = "position:fixed;left:6px;bottom:6px;width:10px;height:10px;border-radius:5px;z-index:2147483647;pointer-events:none;background:#888;opacity:.7";
  const setStatus = (s) => {
    state.status = s;
    dot.style.background = { off: "#888", wait: "#e5a50a", active: "#e01b24", denied: "#333", error: "#333" }[s] || "#888";
  };
  const log = (...a) => console.log("[ta-mic]", ...a);

  const workletSrc = `
    class TaMic extends AudioWorkletProcessor {
      constructor(o) { super(); this.ratio = o.processorOptions.rate / ${RATE}; this.pos = 0; this.buf = new Int16Array(${FRAME}); this.n = 0; this.last = 0; }
      process(inputs) {
        const ch = inputs[0]; if (!ch || !ch.length) return true;
        const len = ch[0].length;
        for (let i = 0; i < len; i++) {
          let s = 0; for (let c = 0; c < ch.length; c++) s += ch[c][i]; s /= ch.length;
          this.pos += 1;
          while (this.pos >= this.ratio) {           // simple decimation/hold resampler to 48 kHz
            this.pos -= this.ratio;
            const v = Math.max(-1, Math.min(1, s));
            this.buf[this.n++] = v < 0 ? v * 0x8000 : v * 0x7fff;
            if (this.n === ${FRAME}) { this.port.postMessage(this.buf.buffer, [this.buf.buffer]); this.buf = new Int16Array(${FRAME}); this.n = 0; }
          }
        }
        return true;
      }
    }
    registerProcessor("ta-mic", TaMic);`;

  function connect() {
    const url = (location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/sockets/mic";
    const ws = new WebSocket(url);
    ws.binaryType = "arraybuffer";
    ws.onopen = () => { log("ws open"); ws.send('{"type":"mic","state":"start"}'); };
    ws.onclose = () => { state.ws = null; if (state.started) setTimeout(connect, 2000); };
    ws.onerror = () => {};
    state.ws = ws;
  }

  async function start() {
    if (state.started) return;
    state.started = true;
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia || !window.AudioWorkletNode) { log("unsupported"); setStatus("error"); return; }
    setStatus("wait");
    try {
      state.stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true, channelCount: 1 } });
    } catch (e) { log("getUserMedia denied", e && e.name); setStatus("denied"); return; }
    try {
      state.ctx = new AudioContext({ latencyHint: "interactive" });
      await state.ctx.audioWorklet.addModule(URL.createObjectURL(new Blob([workletSrc], { type: "application/javascript" })));
      const src = state.ctx.createMediaStreamSource(state.stream);
      state.node = new AudioWorkletNode(state.ctx, "ta-mic", { processorOptions: { rate: state.ctx.sampleRate } });
      state.node.port.onmessage = (e) => {
        const ws = state.ws;
        if (ws && ws.readyState === 1 && ws.bufferedAmount < 19200) ws.send(e.data); // drop when >200 ms queued
      };
      src.connect(state.node);
      const sink = state.ctx.createGain(); sink.gain.value = 0;       // keep the graph pulling, silent
      state.node.connect(sink).connect(state.ctx.destination);
      if (state.ctx.state !== "running") await state.ctx.resume();
      connect();
      setStatus("active");
      log("capture running at", state.ctx.sampleRate, "Hz");
    } catch (e) { log("setup failed", e); setStatus("error"); }
  }

  function stop() {
    state.started = false;
    try { state.ws && state.ws.readyState === 1 && state.ws.send('{"type":"mic","state":"stop"}'); } catch (_) {}
    try { state.ws && state.ws.close(); } catch (_) {}
    try { state.stream && state.stream.getTracks().forEach((t) => t.stop()); } catch (_) {}
    try { state.ctx && state.ctx.close(); } catch (_) {}
    state.ws = state.ctx = state.stream = state.node = null;
    setStatus("off");
  }
  window.taMicStart = start;
  window.taMicStop = stop;

  const onGesture = () => { start(); };
  window.addEventListener("pointerdown", onGesture, { capture: true, passive: true });
  window.addEventListener("pagehide", stop);
  const mount = () => document.body && document.body.appendChild(dot);
  if (document.body) mount(); else document.addEventListener("DOMContentLoaded", mount);
})();
