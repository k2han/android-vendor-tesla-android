(() => {
  const SAMPLE_RATE = 48000;
  const FRAME_SAMPLES = 960;
  let started = false;
  let generation = 0;
  let stream = null;
  let audioContext = null;
  let source = null;
  let captureNode = null;
  let silentGain = null;
  let warned = false;

  function warnOnce(message) {
    if (warned) return;
    warned = true;
    console.warn("[Mic]", message);
  }

  function sendMicState(state) {
    window.sendAudioWebSocket?.({ type: "sendText", text: JSON.stringify({ type: "mic", state }) });
  }

  function stopTracks(mediaStream) {
    if (mediaStream) mediaStream.getTracks().forEach((track) => track.stop());
  }

  function releaseCapture() {
    try { source?.disconnect(); } catch {}
    try { captureNode?.disconnect(); } catch {}
    try { silentGain?.disconnect(); } catch {}
    source = null;
    captureNode = null;
    silentGain = null;
    stopTracks(stream);
    stream = null;
    if (audioContext) {
      const context = audioContext;
      audioContext = null;
      context.close().catch(() => {});
    }
  }

  window.startBrowserMicCapture = function(enabled = true) {
    if (!enabled || started) return;
    if (!navigator.mediaDevices?.getUserMedia || !window.AudioWorkletNode) {
      warnOnce("Microphone capture is unavailable");
      return;
    }

    started = true;
    const request = ++generation;
    const AudioContextClass = window.AudioContext || window.webkitAudioContext;
    if (!AudioContextClass) {
      started = false;
      warnOnce("AudioWorklet is unavailable");
      return;
    }

    try {
      audioContext = new AudioContextClass();
      audioContext.resume().catch(() => {});
    } catch (error) {
      started = false;
      warnOnce(error && error.message ? error.message : "AudioContext creation failed");
      return;
    }

    let mediaRequest;
    try {
      mediaRequest = navigator.mediaDevices.getUserMedia({
        audio: {
          echoCancellation: true,
          noiseSuppression: true,
          autoGainControl: true,
          channelCount: 1,
        },
      });
    } catch (error) {
      started = false;
      releaseCapture();
      warnOnce(error && error.message ? error.message : "Microphone permission was denied");
      return;
    }

    mediaRequest.then(async (mediaStream) => {
      if (!started || request !== generation) {
        stopTracks(mediaStream);
        return;
      }
      stream = mediaStream;
      const worklet = `
        class TeslaMicCapture extends AudioWorkletProcessor {
          constructor() {
            super();
            this.step = sampleRate / ${SAMPLE_RATE};
            this.position = 0;
            this.pending = new Float32Array(0);
            this.frame = new Uint8Array(${FRAME_SAMPLES} * 2);
            this.view = new DataView(this.frame.buffer);
            this.frameOffset = 0;
          }
          process(inputs) {
            const channels = inputs[0];
            if (!channels || !channels.length || !channels[0].length) return true;
            const length = channels[0].length;
            const input = new Float32Array(this.pending.length + length);
            input.set(this.pending);
            for (let channel = 0; channel < channels.length; channel++) {
              for (let i = 0; i < length; i++) input[this.pending.length + i] += channels[channel][i] / channels.length;
            }
            while (this.position + 1 < input.length) {
              const index = Math.floor(this.position);
              const fraction = this.position - index;
              const sample = input[index] * (1 - fraction) + input[index + 1] * fraction;
              const value = Math.max(-1, Math.min(1, sample));
              this.view.setInt16(this.frameOffset * 2, value < 0 ? value * 32768 : value * 32767, true);
              this.frameOffset++;
              this.position += this.step;
              if (this.frameOffset === ${FRAME_SAMPLES}) {
                const buffer = this.frame.buffer;
                this.port.postMessage(buffer, [buffer]);
                this.frame = new Uint8Array(${FRAME_SAMPLES} * 2);
                this.view = new DataView(this.frame.buffer);
                this.frameOffset = 0;
              }
            }
            const consumed = Math.floor(this.position);
            this.pending = input.slice(consumed);
            this.position -= consumed;
            return true;
          }
        }
        registerProcessor("tesla-mic-capture", TeslaMicCapture);
      `;
      const moduleUrl = URL.createObjectURL(new Blob([worklet], { type: "application/javascript" }));
      try {
        await audioContext.audioWorklet.addModule(moduleUrl);
      } finally {
        URL.revokeObjectURL(moduleUrl);
      }
      if (!started || request !== generation) {
        releaseCapture();
        return;
      }

      source = audioContext.createMediaStreamSource(stream);
      captureNode = new AudioWorkletNode(audioContext, "tesla-mic-capture", {
        numberOfInputs: 1,
        numberOfOutputs: 1,
        outputChannelCount: [1],
      });
      silentGain = audioContext.createGain();
      silentGain.gain.value = 0;
      captureNode.port.onmessage = (event) => {
        if (started && event.data instanceof ArrayBuffer) {
          window.sendAudioWebSocket?.({ type: "send", buf: event.data }, [event.data]);
        }
      };
      source.connect(captureNode);
      captureNode.connect(silentGain);
      silentGain.connect(audioContext.destination);
      sendMicState("start");
    }).catch((error) => {
      if (request !== generation) return;
      started = false;
      releaseCapture();
      warnOnce(error && error.message ? error.message : "Microphone permission was denied");
    });
  };

  window.stopBrowserMicCapture = function() {
    if (!started) return;
    started = false;
    generation++;
    sendMicState("stop");
    releaseCapture();
  };

  window.addEventListener("audio-ws-open", () => {
    if (started && captureNode) sendMicState("start");
  });
  window.addEventListener("pagehide", () => window.stopBrowserMicCapture());
})();
