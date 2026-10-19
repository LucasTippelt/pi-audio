class AudioPlaybackProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ringBuf = null;
    this.ringWPos = 0;
    this.ringRPos = 0;
    this.ringSize = 0;
    this.warmupMin = 2400;
    this.warmupDone = false;
    this.dropCount = 0;
    this.initDone = false;
    this.processCount = 0;

    /* Fractional resampler: consume ring slightly slower than real-time so buffer fills */
    this.consumptionRate = 0.997;
    this.phase = 0;
    this.bufIndex = 0;
    this.s0 = 0;
    this.s1 = 0;

    this.port.onmessage = (e) => {
      const d = e.data;
      if (d.type === 'init') {
        this.ringBuf = new Float32Array(d.buffer);
        this.warmupMin = d.warmupMin || 2400;
        this.initDone = true;
        this.port.postMessage({ type: 'ready' });
      } else if (d.type === 'write') {
        if (!this.ringBuf) return;
        const buf = new Float32Array(d.buffer);
        const len = buf.length;
        const cap = this.ringBuf.length - 1 - this.ringSize;
        if (cap <= 0) return;
        const take = Math.min(len, cap);
        if (take <= 0) return;
        const w = this.ringWPos;
        const end = w + take;
        if (end <= this.ringBuf.length) {
          this.ringBuf.set(buf.subarray(0, take), w);
        } else {
          const first = this.ringBuf.length - w;
          this.ringBuf.set(buf.subarray(0, first), w);
          this.ringBuf.set(buf.subarray(first, take), 0);
        }
        this.ringWPos = (w + take) % this.ringBuf.length;
        this.ringSize = Math.min(this.ringSize + take, this.ringBuf.length - 1);
      }
    };
  }

  process(inputs, outputs) {
    const output = outputs[0];
    const out = output[0];
    const len = out.length;

    if (!this.initDone || !this.ringBuf) {
      out.fill(0);
      return true;
    }

    if (!this.warmupDone) {
      if (this.ringSize >= this.warmupMin) {
        this.warmupDone = true;
        this.bufIndex = this.ringRPos;
        this.s0 = this.ringBuf[this.bufIndex];
        this.s1 = this.s0;
        this.phase = 0;
      } else {
        out.fill(0);
        return true;
      }
    }

    const buf = this.ringBuf;
    const cap = buf.length;
    for (let i = 0; i < len; i++) {
      this.phase += this.consumptionRate;
      if (this.phase >= 1 && this.ringSize > 0) {
        this.s0 = this.s1;
        this.s1 = buf[this.bufIndex];
        this.bufIndex = (this.bufIndex + 1) % cap;
        this.ringSize--;
        this.phase -= 1;
      }
      /* Linear interpolation between s0 (previous) and s1 (current) */
      out[i] = this.s0 + (this.s1 - this.s0) * Math.max(0, this.phase);
    }

    if (this.ringSize === 0) {
      this.dropCount += len;
    }

    this.processCount++;
    if (this.processCount % 60 === 0) {
      this.port.postMessage({
        type: 'status',
        size: this.ringSize,
        drops: this.dropCount
      });
      this.dropCount = 0;
    }

    return true;
  }
}

registerProcessor('audio-playback-processor', AudioPlaybackProcessor);
