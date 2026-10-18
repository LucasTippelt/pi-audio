const $ = (selector, root = document) => root.querySelector(selector);
const valueOr = (value, fallback) => value === null || value === undefined ? fallback : value;
const ui = {
  state: null,


  inputs: [],
  outputs: [],
  dspStatusPromise: null,
  metricsPromise: null,
  audioApplyError: '',
  presets: [],
  selectedPreset: 'Default',
  presetVersion: 'staging',
  loadedSnapshot: 'Default Staging',
  currentSource: { name: 'Default', version: 'staging', saved: true },
  activeConfiguration: null,
  lastApplyAttempt: null,
  measurementReady: false,
  applyReady: false,
  expanded: { peq: true },
  delayUnit: 'ms',
  saveTimer: null,
  savePromise: null,
  saveRevision: 0,
  savedRevision: 0,
  toastTimer: null,
  pendingAction: null,
  confirmReturnFocus: null,
  destructiveBusy: false,
  testBusy: false,
};

const defaults = {
    input: 'default', output: 'default', sampleRate: 48000, BufferSize: 512,
    micCount: 2, volume: 65, muted: false,
    crossovers: {
        Woofer: { frequency: 2500, topology: 'Linkwitz-Riley', slope: 24 },
        Tweeter: { frequency: 2500, topology: 'Linkwitz-Riley', slope: 24 },
    },
    bands: { Woofer: [], Tweeter: [] },
    drivers: [
        { name: 'Woofer', delayMs: 0, trimDb: 0, phaseInverted: false },
        { name: 'Tweeter', delayMs: 0, trimDb: 0, phaseInverted: false },
    ],
    limiter: { enabled: true, threshold: -1, release: 80 },
    monitorChannel: 'tweeter',
    measurement: null,
};

async function request(action, body) {
  const payload = body === undefined ? undefined : ['save', 'master', 'apply', 'save-staging', 'save-production'].includes(action)
    ? { ...body, presetName: ui.selectedPreset, presetVersion: ui.presetVersion }
    : body;
  const options = body === undefined ? {} : {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload),
  };
  const response = await fetch(`/api/${encodeURIComponent(action)}`, options);
  const data = await response.json();
  if (Object.prototype.hasOwnProperty.call(data, 'currentJsonWritten')) {
    setCurrentJsonStatus(data.currentJsonWritten === true);
  }
  if (!response.ok) {
    if (!Object.prototype.hasOwnProperty.call(data, 'currentJsonWritten')
      && ['save', 'master', 'apply', 'save-staging', 'save-production', 'restore-defaults', 'measure'].includes(action)) {
      setCurrentJsonStatus(false);
    }
    const error = new Error(data.error || `Request failed (${response.status})`);
    error.currentJsonWritten = data.currentJsonWritten;
    error.lastLoad = data.lastLoad;
    error.lastApplyAttempt = data.lastApplyAttempt;
    throw error;
  }
  return data;
}

function notify(message, bad = false) {
  const toast = $('#toast');
  toast.textContent = message;
  toast.classList.toggle('error', bad);
  toast.classList.add('visible');
  clearTimeout(ui.toastTimer);
  ui.toastTimer = setTimeout(() => toast.classList.remove('visible'), 3400);
}

function setSaveStatus(text, pending = false) {
  const el = $('#save-status');
  el.textContent = text;
  el.classList.toggle('pending', pending);
  if (pending) {
    el.innerHTML = text + ' <button type="button" class="status-clear">\u2715</button>';
    el.querySelector('.status-clear').addEventListener('click', () => {
      el.textContent = 'Changes save to the active configuration';
      el.classList.remove('pending');
    });
  }
}

function setCurrentJsonStatus(written, pending = false) {
  const status = $('#current-json-status');
  status.textContent = written ? 'Written to current.json' : pending ? 'current.json write pending' : 'current.json not verified';
  status.classList.toggle('is-pending', pending);
  status.classList.toggle('is-error', !written && !pending);
}

function updateLoadStatus(snapshot) {
  ui.loadedSnapshot = snapshot;
  if (!snapshot) {
    $('#load-status').textContent = 'No snapshot loaded yet';
    return;
  }
  const time = snapshot.at
    ? new Date(snapshot.at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', hour12: false })
    : new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', hour12: false });
  $('#load-status').textContent = `Loaded: ${snapshot.snapshot} at ${time}`;
}

function updateViewStatus() {
  const dirty = ui.saveRevision !== ui.savedRevision;
  const source = ui.currentSource;
  const label = dirty || !source || !source.saved
    ? `unsaved current.json (based on ${source.name} ${source.version[0].toUpperCase()}${source.version.slice(1)})`
    : `${source.name} ${source.version[0].toUpperCase()}${source.version.slice(1)}`;
  $('#view-status').textContent = `Viewing: ${label}`;
}

function updateActiveStatus(active = ui.activeConfiguration) {
  ui.activeConfiguration = active;
  const attempt = ui.lastApplyAttempt;
  const attemptedName = attempt ? `${attempt.name} ${attempt.version[0].toUpperCase()}${attempt.version.slice(1)}` : '';
  const activeName = active
    ? `${active.name} ${active.version[0].toUpperCase()}${active.version.slice(1)} ${active.saved ? 'saved' : 'unsaved'}`
    : '';
  const label = attempt && !attempt.success
    ? `apply failed for ${attemptedName}${activeName ? ' \u00b7 last successful: ' + activeName : ''}`
    : activeName || 'not applied this session';
  $('#active-status').textContent = `Active: ${label}`;
}

function updateApplyButton() {
  const button = $('#apply-button');
  updateViewStatus();
  const source = $('#view-status').textContent.replace(/^Viewing:\s*/, '');
  button.innerHTML = '<span aria-hidden="true">\u21bb</span> Apply current.json to engine';
  button.title = `Apply current.json (${source}) to the audio engine${ui.applyReady ? '' : '. Configure PI_AUDIO_APPLY_BIN to enable the adapter'}`;
}

function queueSave() {
  ui.saveRevision += 1;
  clearTimeout(ui.saveTimer);
  setCurrentJsonStatus(false, true);
  setSaveStatus('Unsaved changes', true);
  if (ui.currentSource) ui.currentSource.saved = false;
  updateApplyButton();
  drawCurves();
  ui.saveTimer = setTimeout(() => {
    ui.saveTimer = null;
    saveState();
  }, 80);
}

async function saveState() {
  if (ui.savePromise) return ui.savePromise;
  const pending = (async () => {
    do {
      const revision = ui.saveRevision;
      const state = JSON.parse(JSON.stringify(ui.state));
      setSaveStatus('Saving changes', false);
      try {
        const result = await request('save', state);
        if (result.currentJsonWritten !== true) throw new Error('The server did not verify writing current.json.');
        ui.currentSource = result.currentSource || { name: ui.selectedPreset, version: ui.presetVersion, saved: false };
        if (ui.presetVersion === 'production') ui.currentSource.saved = false;
        if (result.activeConfiguration) updateActiveStatus(result.activeConfiguration);
        ui.savedRevision = revision;
      } catch (error) {
        if (revision !== ui.saveRevision) continue;
        setSaveStatus('Save failed', true);
        notify(error.message, true);
        return false;
      }
    } while (ui.savedRevision !== ui.saveRevision);
    setCurrentJsonStatus(true);
    updateApplyButton();
    return true;
  })();
  ui.savePromise = pending;
  try {
    await ui.savePromise;
  } finally {
    if (ui.savePromise === pending) ui.savePromise = null;
  }
  return ui.savedRevision === ui.saveRevision;
}
function renderInputs() {
  const select = $('#audio-input');
  const inputs = [...new Set(['default', ...ui.inputs, ui.state.input])];
  select.innerHTML = inputs.map((item) => `<option value="${escapeHtml(item)}">${escapeHtml(item)}</option>`).join('');
  select.value = ui.state.input;
  $('#sample-rate').value = String(ui.state.sampleRate);
  $('#sample-rate').disabled = false;
  $('#buffer-size').value = String(ui.state.BufferSize);
  $('#buffer-size').disabled = false;
  $('#input-note').textContent = ui.presetVersion === 'production'
    ? 'Input, sample rate, and buffer size remain configurable in production.'
    : 'Input, sample rate, and buffer size are saved to staging; apply the version to load it in the engine.';
}

function renderMicCount() {
  const select = $('#mic-count');
  const count = ui.state.micCount || 2;
  select.value = String(count);
  $('#mic-note').textContent = `${count} measurement microphone input(s) configured.`;
}

function renderMonitorChannel() {
  const select = $('#monitor-select');
  const count = ui.state.micCount || 2;
  let options = '<option value="none">\u2014 No monitoring \u2014</option>';
  options += '<option value="pgm-l">PGM Left (DSP)</option>';
  options += '<option value="pgm-r">PGM Right (passthrough)</option>';
  options += '<option value="tweeter">Tweeter output</option>';
  options += '<option value="woofer">Woofer output</option>';
  for (let i = 1; i <= count; i++) {
    options += `<option value="mic-${i}">Mic ${i}</option>`;
  }
  select.innerHTML = options;
  const channel = ui.state.monitorChannel || 'none';
  select.value = channel;
  $('#monitor-note').textContent = 'Select one channel to listen in the browser. Non-realtime monitoring.';
}

function renderOutputs() {
  const select = $('#audio-output');
  const outputs = [...new Set(['default', ...ui.outputs, ui.state.output])];
  select.innerHTML = outputs.map((item) => `<option value="${escapeHtml(item)}">${escapeHtml(item)}</option>`).join('');
  select.value = ui.state.output;
  $('#output-note').textContent = 'For debugging only; remove after development.';
}

function escapeHtml(value) {
  return String(value).replace(/[&<>\"']/g, (character) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[character]);
}

function displayDelay(ms) {
  if (ui.delayUnit === 'cm') return (ms * 34.3).toFixed(1);
  return Number(ms).toFixed(2);
}

function delayToMs(value) {
  if (ui.delayUnit === 'cm') return Number(value) / 34.3;
  return Number(value);
}

function renderDrivers() {
  $('#driver-list').innerHTML = ui.state.drivers.map((driver, index) => `
    <div class="driver-row" data-driver="${index}">
      <strong>${escapeHtml(driver.name)}</strong>
      <label class="compact-input"><input data-driver-delay="${index}" type="number" min="0" max="${ui.delayUnit === 'ms' ? '20' : '686'}" step="${ui.delayUnit === 'ms' ? '0.01' : '0.1'}" value="${displayDelay(driver.delayMs)}" aria-label="${escapeHtml(driver.name)} delay"><span>${ui.delayUnit}</span></label>
      <label class="compact-input"><input data-driver-trim="${index}" type="number" min="-24" max="12" step="0.1" value="${Number(driver.trimDb).toFixed(1)}" aria-label="${escapeHtml(driver.name)} trim"><span>dB</span></label>
      <label class="switch phase-toggle" title="Toggle 180-degree phase inversion for ${escapeHtml(driver.name)}"><input data-driver-phase="${index}" type="checkbox" ${driver.phaseInverted ? 'checked' : ''} aria-label="Invert phase 180 degrees for ${escapeHtml(driver.name)}"><span></span></label>
      <label class="compact-input"><input data-driver-duration="${index}" type="number" min="1" max="60" step="1" placeholder="3" aria-label="${escapeHtml(driver.name)} test duration"><span>s</span></label>
      <button class="test-speaker" type="button" data-test-speaker="${escapeHtml(driver.name)}" aria-label="Test speaker ${escapeHtml(driver.name)}" title="Attempt speaker test for ${escapeHtml(driver.name)}">Test</button>
    </div>`).join('');
}

function renderCrossovers() {
  const names = ['Woofer', 'Tweeter'];
  $('#crossover-list').innerHTML = names.map((name) => {
    const crossover = ui.state.crossovers[name];
    const filterType = name === 'Woofer' ? 'LOW-PASS' : 'HIGH-PASS';
    return `<article class="crossover-card">
      <div class="crossover-card-head"><span>${filterType}</span><strong>${name}</strong></div>
      <div class="crossover-card-fields">
        <label class="field-label">FREQUENCY <input class="crossover-number" data-crossover-frequency="${name}" type="number" min="30" max="500" step="1" value="${crossover.frequency}" aria-label="${name} crossover frequency"> Hz</label>
        <label class="field-label">TOPOLOGY <select class="select-control" data-crossover-topology="${name}">${['Linkwitz-Riley', 'Butterworth', 'Bessel'].map((t) => `<option value="${t}" ${crossover.topology === t ? 'selected' : ''}>${t}</option>`).join('')}</select></label>
        <label class="field-label">SLOPE <select class="select-control" data-crossover-slope="${name}">${[12, 18, 24, 36, 48].map((slope) => `<option value="${slope}" ${crossover.slope === slope ? 'selected' : ''}>${slope} dB/oct</option>`).join('')}</select></label>
      </div>
    </article>`;
  }).join('');
}

function renderBands() {
  const drivers = { woofer: 'Woofer', tweeter: 'Tweeter' };
  Object.entries(drivers).forEach(([type, driverName]) => {
    const bands = ui.state.bands[driverName];
    $(`#${type}-band-list`).innerHTML = bands.map((band, index) => `
      <article class="band-row ${band.enabled ? '' : 'is-bypassed'}" data-band="${escapeHtml(band.id)}">
        <span class="band-index">${String(index + 1).padStart(2, '0')}</span>
        <label class="band-frequency"><span>FREQ</span><input data-band-driver="${driverName}" data-band-frequency="${escapeHtml(band.id)}" type="number" min="20" max="20000" value="${band.frequency}" aria-label="${driverName} band ${index + 1} center frequency"><b>Hz</b></label>
        <label class="band-q"><span>Q</span><input data-band-driver="${driverName}" data-band-q="${escapeHtml(band.id)}" type="number" min="0.1" max="18" step="0.1" value="${band.q}" aria-label="${driverName} band ${index + 1} Q factor"></label>
        <label class="band-gain"><span>GAIN</span><input data-band-driver="${driverName}" data-band-gain="${escapeHtml(band.id)}" type="number" min="-18" max="18" step="0.1" value="${Number(band.gain).toFixed(1)}" aria-label="${driverName} band ${index + 1} gain"><b>dB</b></label>
        <button class="band-bypass ${band.enabled ? '' : 'active'}" data-band-driver="${driverName}" data-band-toggle="${escapeHtml(band.id)}" type="button" aria-label="${band.enabled ? 'Bypass' : 'Enable'} ${driverName} band ${index + 1}" aria-pressed="${!band.enabled}"><span></span></button>
        <button class="band-remove" data-band-driver="${driverName}" data-band-remove="${escapeHtml(band.id)}" type="button" aria-label="Remove ${driverName} band ${index + 1}">\u00d7</button>
      </article>`).join('');
    $(`#${type}-band-count`).textContent = String(bands.length).padStart(2, '0');
    $(`[data-add-band="${type}"]`).disabled = bands.length >= 12;
  });
}

function renderCollapseState() {
  document.querySelectorAll('[data-collapse]').forEach((button) => {
    const section = button.dataset.collapse;
    const expanded = ui.expanded[section];
    $(`#${button.getAttribute('aria-controls')}`).hidden = !expanded;
    button.textContent = expanded ? '-' : '+';
    button.setAttribute('aria-expanded', String(expanded));
    button.setAttribute('aria-label', `${expanded ? 'Minimize' : 'Expand'} parametric EQ controls`);
  });
}

function renderPresets() {
  updateApplyButton();
  const list = $('#preset-list');
  list.innerHTML = ui.presets.map((name) => `<div class="preset-item ${name === ui.selectedPreset ? 'active' : ''}">
    <strong>${escapeHtml(name)}</strong>
    <button type="button" data-preset-select="${escapeHtml(name)}" data-preset-version="staging" aria-pressed="${name === ui.selectedPreset && ui.presetVersion === 'staging'}">Staging</button>
    <button type="button" data-preset-select="${escapeHtml(name)}" data-preset-version="production" aria-pressed="${name === ui.selectedPreset && ui.presetVersion === 'production'}">Production</button>
    ${name === 'Default' ? '' : `<button type="button" class="preset-delete" data-preset-delete="${escapeHtml(name)}" aria-label="Delete ${escapeHtml(name)}">\u00d7</button>`}
  </div>`).join('');
  $('#preset-mode').textContent = `${ui.selectedPreset} \u00b7 ${ui.presetVersion}${ui.presetVersion === 'production' ? ' (tuning read-only)' : ''}`;
  $('#promote-preset').disabled = ui.presetVersion !== 'staging';
  $('#copy-production-to-staging').disabled = ui.presetVersion !== 'production';
  $('#apply-button').disabled = false;
  $('#save-staging').disabled = false;
  $('#save-staging').textContent = ui.presetVersion === 'production' ? 'Save production' : 'Save staging';
  $('#measure-button').disabled = false;
  document.querySelectorAll('main .overview input, main .overview select, main .overview button, main .control-section input, main .control-section select, main .control-section button').forEach((control) => {
    if (ui.presetVersion === 'production') {
      if (['audio-input', 'sample-rate', 'buffer-size'].includes(control.id)) {
        control.disabled = false;
        delete control.dataset.productionDisabled;
        return;
      }
      if (control.dataset.productionDisabled === undefined) control.dataset.productionDisabled = String(control.disabled);
      control.disabled = true;
    } else if (control.dataset.productionDisabled !== undefined) {
      control.disabled = control.dataset.productionDisabled === 'true';
      delete control.dataset.productionDisabled;
    }
  });
}

function updatePresetSelection() {
  renderPresets();
}

function measurementResponse() {
  const measurement = ui.state.measurement;
  return measurement && measurement.responses ? measurement.responses : null;
}

function renderState(includePresets = true) {
  renderInputs();
  renderOutputs();
  renderMicCount();
  renderMonitorChannel();
  renderBands();
  renderDrivers();
  renderCrossovers();
  if (includePresets) renderPresets();
  renderCollapseState();
  $('#master-volume').value = ui.state.volume;
  $('#volume-value').textContent = String(ui.state.volume);
  $('#mute-button').classList.toggle('muted', ui.state.muted);
  $('#mute-button').setAttribute('aria-pressed', String(ui.state.muted));
  $('#mute-button').textContent = ui.state.muted ? 'Muted' : 'Mute';
  $('#limiter-enabled').checked = ui.state.limiter.enabled;
  $('#limiter-threshold').value = ui.state.limiter.threshold;
  $('#limiter-release').value = ui.state.limiter.release;
  $('#curve-caption').textContent = ui.state.measurement
    ? `MEASURED ${new Date(ui.state.measurement.measuredAt).toLocaleString()}`
    : '';
  drawCurves();
}

function addBand(driverName) {
  const bands = ui.state.bands[driverName];
  if (bands.length >= 12) return;
  bands.push({ id: `b${Date.now().toString(36)}`, enabled: true, frequency: driverName === 'Woofer' ? 50 : 1000, q: 1, gain: 0 });
  renderBands();
  queueSave();
}
function setMaster() {
  clearTimeout(ui.saveTimer);
  ui.saveRevision += 1;
  setCurrentJsonStatus(false, true);
  setSaveStatus('Saving volume to current.json', true);
  if (ui.currentSource) ui.currentSource.saved = false;
  updateApplyButton();
  request('master', ui.state).then(async (result) => {
    ui.savedRevision = ui.saveRevision;
    if (result.currentSource) ui.currentSource = result.currentSource;
    updateApplyButton();
    if (!result.alsaApplied) notify(result.alsaMessage, true);
    setSaveStatus(result.alsaApplied ? 'Volume updated' : 'ALSA mixer unavailable', !result.alsaApplied);
  }).catch((error) => {
    setSaveStatus('Volume save failed', true);
    notify(error.message, true);
  });
}

function setAudioParameterError(selector, message) {
  const field = $(selector);
  field.classList.toggle('is-error', Boolean(message));
  field.setAttribute('aria-invalid', String(Boolean(message)));
  field.title = message;
}

function setAudioApplyFailure(message) {
  ui.audioApplyError = message || 'Audio engine apply failed.';
  setAudioParameterError('#sample-rate', ui.audioApplyError);
  setAudioParameterError('#buffer-size', ui.audioApplyError);
}

function dspSettingsMatch(status) {
  return Boolean(status && status.connected
    && status.sampleRate === ui.state.sampleRate
    && status.BufferSize === ui.state.BufferSize);
}

function drawCurve(driver, canvasId) {
  const canvas = $(canvasId);
  if (!canvas) return;
  const rect = canvas.getBoundingClientRect();
  if (!rect.width || !rect.height) return;
  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(rect.width * dpr);
  canvas.height = Math.round(rect.height * dpr);
  const ctx = canvas.getContext('2d');
  ctx.scale(dpr, dpr);
  const width = rect.width;
  const height = rect.height;
  const pad = { left: 43, right: 10, top: 8, bottom: 22 };
  const chartW = width - pad.left - pad.right;
  const laneHeight = height - pad.top - pad.bottom;
  const minHz = driver === 'Tweeter' ? 1000 : 20;
  const maxHz = 20000;
  const minDb = -24;
  const maxDb = 12;
  const xFor = (hz) => pad.left + (Math.log10(hz / minHz) / Math.log10(maxHz / minHz)) * chartW;
  const yFor = (db) => pad.top + ((maxDb - db) / (maxDb - minDb)) * laneHeight;
  ctx.clearRect(0, 0, width, height);
  ctx.font = '10px "DM Mono", monospace';
  ctx.lineWidth = 1;
  const bandResponse = (hz, driverName) => {
    const logHz = Math.log2(hz);
    let db = 0;
    for (const band of ui.state.bands[driverName]) {
      if (!band.enabled || band.gain === 0) continue;
      const distance = (logHz - Math.log2(band.frequency)) * band.q;
      db += band.gain * Math.exp(-2 * distance * distance);
    }
    return db;
  };
  const filterResponse = (hz, crossover, lowPass) => {
    const ratio = lowPass ? hz / crossover.frequency : crossover.frequency / hz;
    const order = crossover.slope / 6;
    const power = Math.pow(ratio, order * (crossover.topology === 'Linkwitz-Riley' ? 1 : 2));
    return crossover.topology === 'Linkwitz-Riley'
      ? -20 * Math.log10(1 + power)
      : -10 * Math.log10(1 + power);
  };
  const wooferName = 'Woofer';
  const tweeterName = 'Tweeter';
  const getResponse = (hz) => {
    if (driver === 'Combined') {
      const wooferDb = bandResponse(hz, wooferName) + filterResponse(hz, ui.state.crossovers[wooferName], true);
      const tweeterDb = bandResponse(hz, tweeterName) + filterResponse(hz, ui.state.crossovers[tweeterName], false);
      const wooferMagnitude = Math.pow(10, wooferDb / 20);
      const tweeterMagnitude = Math.pow(10, tweeterDb / 20);
      const bothLinkwitzRiley = ui.state.crossovers[wooferName].topology === 'Linkwitz-Riley'
        && ui.state.crossovers[tweeterName].topology === 'Linkwitz-Riley';
      const combinedDb = bothLinkwitzRiley
        ? 20 * Math.log10(wooferMagnitude + tweeterMagnitude)
        : 10 * Math.log10(wooferMagnitude ** 2 + tweeterMagnitude ** 2);
      return Math.max(minDb, Math.min(maxDb, combinedDb));
    }
    const crossover = ui.state.crossovers[driver];
    const isLowPass = driver === wooferName;
    return Math.max(minDb, Math.min(maxDb, bandResponse(hz, driver) + filterResponse(hz, crossover, isLowPass)));
  };
  const drawSeries = (points, color, dashed = false) => {
    if (!points || !points.length) return;
    ctx.save();
    ctx.beginPath();
    ctx.rect(pad.left, pad.top, chartW, laneHeight);
    ctx.clip();
    ctx.beginPath();
    ctx.strokeStyle = color;
    ctx.lineWidth = 2.2;
    ctx.setLineDash(dashed ? [5, 4] : []);
    points.forEach((point, index) => {
      const x = xFor(point.hz);
      const y = yFor(point.db);
      if (index === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    });
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.restore();
  };
  const frequencyLabels = new Map([
    [20, '20 Hz'], [50, '50'], [100, '100'], [200, '200'], [500, '500'],
    [1000, '1k'], [2000, '2k'], [5000, '5k'], [10000, '10k'], [20000, '20k'],
  ]);
  for (let db = maxDb; db >= minDb; db -= 3) {
    const y = yFor(db);
    const labeled = db % 6 === 0;
    ctx.strokeStyle = db === 0 ? '#a8aaa3' : labeled ? '#deded6' : '#eeede7';
    ctx.setLineDash(db === 0 ? [4, 4] : []);
    ctx.beginPath(); ctx.moveTo(pad.left, y); ctx.lineTo(width - pad.right, y); ctx.stroke();
    ctx.setLineDash([]);
    if (labeled) {
      ctx.fillStyle = '#999b93';
      ctx.textAlign = 'right';
      ctx.fillText(String(db), pad.left - 7, y + 3);
    }
  }
  [20, 30, 40, 50, 70, 100, 200, 300, 500, 700, 1000, 2000, 3000, 5000, 7000, 10000, 20000].filter((hz) => hz >= minHz).forEach((hz) => {
    const x = xFor(hz);
    ctx.strokeStyle = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000].includes(hz) ? '#e3e2da' : '#f0efe9';
    ctx.beginPath(); ctx.moveTo(x, pad.top); ctx.lineTo(x, pad.top + laneHeight); ctx.stroke();
  });
  ctx.fillStyle = '#66695e';
  ctx.textAlign = 'left';
  ctx.font = '10px "DM Mono", monospace';
  ctx.fillText(driver === 'Combined' ? 'COMBINED' : driver.toUpperCase(), pad.left + 5, pad.top + 12);
  const pointCount = Math.max(720, Math.ceil(chartW * dpr * 3));
  const filterCurve = Array.from({ length: pointCount }, (_, pointIndex) => {
    const hz = minHz * ((maxHz / minHz) ** (pointIndex / (pointCount - 1)));
    return { hz, db: getResponse(hz) };
  });
  drawSeries(filterCurve, '#8a9b4a');
  const measData = measurementResponse();
  if (measData) {
    drawSeries(measData, '#e16343', true);
  }
  ctx.fillStyle = '#999b93';
  ctx.font = '9px "DM Mono", monospace';
  frequencyLabels.forEach((label, hz) => {
    if (hz < minHz) return;
    ctx.textAlign = hz === minHz ? 'left' : hz === maxHz ? 'right' : 'center';
    ctx.fillText(label, xFor(hz), pad.top + laneHeight + 13);
  });
  ctx.setLineDash([]);
}

function drawCurves() {
  drawCurve('Combined', '#response-chart-combined');
  drawCurve('Woofer', '#response-chart-woofer');
  drawCurve('Tweeter', '#response-chart-tweeter');
}

async function refreshMetrics() {
  if (ui.metricsPromise) return ui.metricsPromise;
  const metricsRequest = (async () => {
    try {
      const data = await request('metrics');
      $('#metric-cpu').innerHTML = `${valueOr(data.cpu, '--')}<small>%</small>`;
      $('#cpu-bar').style.width = `${valueOr(data.cpu, 0)}%`;
      $('#metric-temp').innerHTML = `${valueOr(data.temperature, '--')}<small>\u00b0C</small>`;
      $('#temp-mark').style.setProperty('--heat', `${Math.max(0, Math.min(100, ((valueOr(data.temperature, 0)) - 35) * 3))}%`);
      $('#metric-xruns').innerHTML = `${valueOr(data.xruns, '--')}<small> count</small>`;
      $('#xrun-foot').textContent = data.xruns === null ? 'counter unavailable' : 'since engine start';
      const status = $('#metrics-status');
      const cpuUnavailable = data.cpu === null || data.cpu === undefined;
      status.lastChild.textContent = cpuUnavailable ? 'CPU UNAVAILABLE' : data.cpuSource === 'loadavg' ? 'LOAD AVG' : 'LIVE';
      status.title = cpuUnavailable ? data.cpuMessage : data.cpuSource === 'loadavg'
        ? 'Approximate 1-minute load average normalized by the number of logical CPUs'
        : '';
      status.classList.toggle('is-error', cpuUnavailable);
    } catch (error) {
      $('#metric-cpu').innerHTML = '--<small>%</small>';
      $('#cpu-bar').style.width = '0%';
      $('#metric-temp').innerHTML = '--<small>\u00b0C</small>';
      $('#metric-xruns').innerHTML = '--<small> count</small>';
      const status = $('#metrics-status');
      status.lastChild.textContent = 'OFFLINE';
      status.title = error.message;
      status.classList.add('is-error');
    } finally {
      ui.metricsPromise = null;
    }
  })();
  ui.metricsPromise = metricsRequest;
  return metricsRequest;
}

async function refreshDspStatus() {
  if (ui.dspStatusPromise) return ui.dspStatusPromise;
  const status = $('#dsp-status');
  const statusRequest = (async () => {
    try {
      const data = await request('dsp-status');
      console.log('[pi-audio] DSP status response:', data);
      const sampleRate = data.sampleRate ? ` \u00b7 ${(data.sampleRate / 1000).toFixed(data.sampleRate % 1000 ? 1 : 0)} kHz` : '';
      status.lastElementChild.textContent = `DSP connection: ${data.connected ? 'Connected' : 'Unavailable'}${sampleRate}`;
      status.title = data.message || '';
      status.classList.toggle('is-error', !data.connected);
      status.classList.toggle('is-connected', data.connected);
      if (ui.audioApplyError) {
        setAudioParameterError('#sample-rate', ui.audioApplyError);
        setAudioParameterError('#buffer-size', ui.audioApplyError);
      } else if (!data.connected) {
        const message = data.message || 'DSP is unavailable; sample rate and buffer size could not be confirmed.';
        setAudioParameterError('#sample-rate', message);
        setAudioParameterError('#buffer-size', message);
      } else {
        setAudioParameterError('#sample-rate', data.sampleRate === ui.state.sampleRate
          ? '' : `Requested ${ui.state.sampleRate} Hz; DSP is running at ${data.sampleRate} Hz.`);
        setAudioParameterError('#buffer-size', data.BufferSize === ui.state.BufferSize
          ? '' : `Requested ${ui.state.BufferSize} samples; DSP is running with ${data.BufferSize} samples.`);
      }
      return data;
    } catch (error) {
      status.lastElementChild.textContent = 'DSP connection: Unavailable';
      status.title = error.message;
      status.classList.add('is-error');
      status.classList.remove('is-connected');
      console.error("[pi-audio] DSP status error:", error);
      const message = ui.audioApplyError || `DSP status unavailable: ${error.message}`;
      setAudioParameterError('#sample-rate', message);
      setAudioParameterError('#buffer-size', message);
      return null;
    } finally {
      ui.dspStatusPromise = null;
    }
  })();
  ui.dspStatusPromise = statusRequest;
  return statusRequest;
}
function showMeasurement(active) {
  $('#measure-overlay').hidden = !active;
  $('#measure-button').disabled = active;
}

async function measureRoom() {
  try {
    const issues = [];
    if (!ui.measurementReady) issues.push('Measurement adapter not configured. Set PI_AUDIO_MEASURE_BIN.');
    const dspStatus = await refreshDspStatus();
    if (!dspSettingsMatch(dspStatus)) {
      issues.push(!dspStatus || !dspStatus.connected
        ? 'DSP connection is unavailable'
        : 'DSP did not confirm the requested sample rate and buffer size');
    }
    if (issues.length) {
      setSaveStatus(`Room measurement failed: ${issues.join('; ')}`, true);
      notify(`Room measurement failed: ${issues.join('; ')}`, true);
      return;
    }
    clearTimeout(ui.saveTimer);
    try {
      await request('save', ui.state);
    } catch (error) {
      notify(error.message, true);
      return;
    }
    setSaveStatus('Running room measurement', true);
    const result = await request('measure', {});
    ui.state = result.state;
    renderState();
    setSaveStatus('Measurement and configuration saved');
    setCurrentJsonStatus(true);
    $('#measure-title').textContent = 'Room profile ready';
    $('#measure-detail').textContent = 'Measured response has been saved and added to the graph.';
    showMeasurement(true);
    notify('Room measurement complete');
    setTimeout(() => showMeasurement(false), 3000);
  } catch (error) {
    setSaveStatus(`Room measurement failed: ${error.message || 'Unknown error'}`, true);
    notify(`Room measurement failed: ${error.message || 'Unknown error'}`, true);
  }
}

async function applyToEngine() {
  const button = $('#apply-button');
  clearTimeout(ui.saveTimer);
  if (ui.savePromise) await ui.savePromise;
  const source = ui.saveRevision === ui.savedRevision && ui.currentSource && ui.currentSource.saved
    ? `${ui.currentSource.name} ${ui.currentSource.version[0].toUpperCase()}${ui.currentSource.version.slice(1)}`
    : 'unsaved current.json';
  updateLoadStatus({ snapshot: source, at: new Date().toISOString() });
  button.disabled = true;
  button.textContent = 'Applying\u2026';
  setSaveStatus('Applying configuration to audio engine', true);
  try {
    const result = await request('apply', ui.state);
    ui.savedRevision = ui.saveRevision;
    ui.activeConfiguration = result.activeConfiguration || ui.activeConfiguration;
    ui.lastApplyAttempt = result.lastApplyAttempt || ui.lastApplyAttempt;
    ui.currentSource = result.currentSource || ui.currentSource;
    if (result.activeConfiguration) updateActiveStatus(result.activeConfiguration);
    if (result.lastLoad) updateLoadStatus(result.lastLoad);
    updateActiveStatus();
    updateApplyButton();
    ui.audioApplyError = '';
    setSaveStatus('Configuration applied to audio engine');
    notify('Audio engine configuration applied');
    refreshDspStatus().then((dspStatus) => {
      if (!dspSettingsMatch(dspStatus)) {
        const message = !dspStatus || !dspStatus.connected
          ? `DSP is unavailable${(dspStatus && dspStatus.message) || $('#dsp-status').title ? ': ' + ((dspStatus && dspStatus.message) || $('#dsp-status').title) : '.'}`
          : `DSP did not confirm the requested sample rate and buffer size${dspStatus.message ? ': ' + dspStatus.message : '.'}`;
        setAudioApplyFailure(message);
        setSaveStatus('Audio settings could not be confirmed', true);
        notify(message, true);
      }
    });
  } catch (error) {
    if (error.lastLoad) updateLoadStatus(error.lastLoad);
    if (error.lastApplyAttempt) {
      ui.lastApplyAttempt = error.lastApplyAttempt;
      updateActiveStatus();
    }
    if (error.currentJsonWritten === true) {
      ui.savedRevision = ui.saveRevision;
      updateApplyButton();
    }
    setAudioApplyFailure(error.message);
    setSaveStatus('Audio engine apply failed', true);
    notify(error.message, true);
  } finally {
    renderPresets();
  }
}

function openPresetModal() {
  $('#preset-modal').hidden = false;
  $('#preset-name').focus();
}

function closePresetModal() { $('#preset-modal').hidden = true; }

function openConfirmAction(action, trigger) {
  const dialog = $('#confirm-action-modal');
  const isDelete = action === 'preset-delete-all';
  const isCopyToStaging = action === 'preset-copy-production-to-staging';
  ui.pendingAction = action;
  ui.confirmReturnFocus = trigger;
  $('#confirm-action-title').textContent = isDelete ? 'Delete all user presets?' : isCopyToStaging ? 'Replace staging with production?' : 'Restore factory defaults?';
  $('#confirm-action-description').textContent = isDelete
    ? 'This permanently deletes every user preset. The protected Default preset is kept, and its staging version becomes active.'
    : isCopyToStaging
      ? `This overwrites ${ui.selectedPreset} staging with its production snapshot and writes the copied settings to current.json. It does not apply them to the engine.`
      : 'This replaces the active audio configuration with factory defaults. Saved presets will not be deleted.';
  $('#confirm-action').textContent = isDelete ? 'Delete all user presets' : isCopyToStaging ? 'Copy to staging' : 'Restore defaults';
  dialog.hidden = false;
  $('#cancel-confirm-action').focus();
}

function closeConfirmAction() {
  $('#confirm-action-modal').hidden = true;
  ui.pendingAction = null;
  if (ui.confirmReturnFocus) ui.confirmReturnFocus.focus();
  ui.confirmReturnFocus = null;
}

async function performDestructiveAction() {
  if (!ui.pendingAction || ui.destructiveBusy) return;
  const action = ui.pendingAction;
  const confirmButton = $('#confirm-action');
  const isDelete = action === 'preset-delete-all';
  const isCopyToStaging = action === 'preset-copy-production-to-staging';
  const confirmLabel = isDelete ? 'Delete all user presets' : isCopyToStaging ? 'Copy to staging' : 'Restore defaults';
  const cancelButton = $('#cancel-confirm-action');
  const closeButton = $('#close-confirm-action');
  ui.destructiveBusy = true;
  confirmButton.disabled = true;
  cancelButton.disabled = true;
  closeButton.disabled = true;
  $('#delete-all-presets').disabled = true;
  $('#copy-production-to-staging').disabled = true;
  $('#restore-defaults').disabled = true;
  confirmButton.textContent = isDelete ? 'Deleting\u2026' : isCopyToStaging ? 'Copying\u2026' : 'Restoring\u2026';
  if (action === 'restore-defaults') setSaveStatus('Restoring factory defaults', true);
  clearTimeout(ui.saveTimer);

  try {
    if (ui.savePromise) await ui.savePromise;
    let result;
    if (action === 'preset-delete-all') {
      result = await request(action, {});
      ui.presets = result.presets;
      ui.selectedPreset = result.selectedPreset || 'Default';
      ui.presetVersion = result.presetVersion || 'staging';
      ui.currentSource = result.currentSource || ui.currentSource;
      if (result.activeConfiguration) updateActiveStatus(result.activeConfiguration);
      if (result.lastLoad) updateLoadStatus(result.lastLoad);
      if (result.state) ui.state = result.state;
      renderState();
      setSaveStatus(result.deleted ? 'User presets deleted' : 'Preset deletion incomplete', !result.deleted);
      const issues = [];
      if (!result.deleted) issues.push(`Could not delete: ${result.deleteErrors.join(', ')}`);
      if (result.currentJsonWritten !== true) issues.push('current.json write was not verified');
      notify(issues.length ? `Presets deleted \u00b7 ${issues.join('; ')}` : 'All user presets deleted; Default was kept', issues.length > 0);
    } else if (isCopyToStaging) {
      if (ui.saveRevision !== ui.savedRevision && !(await saveState())) throw new Error('Could not save production settings before copying to staging.');
      result = await request(action, { name: ui.selectedPreset, version: ui.presetVersion });
      ui.state = result.state;
      ui.presetVersion = result.presetVersion;
      ui.currentSource = result.currentSource || { name: ui.selectedPreset, version: 'staging', saved: true };
      if (result.activeConfiguration) updateActiveStatus(result.activeConfiguration);
      ui.savedRevision = ui.saveRevision;
      renderState();
      if (result.lastLoad) updateLoadStatus(result.lastLoad);
      if (result.currentJsonWritten !== true) throw new Error('The server did not verify writing current.json.');
      setSaveStatus('Production copied to staging and written to current.json');
      notify('Production copied to staging; apply current.json to engine when ready');
    } else {
      result = await request(action, {});
      ui.state = result.state;
      ui.selectedPreset = 'Default';
      ui.presetVersion = 'staging';
      ui.currentSource = result.currentSource || { name: 'Default', version: 'staging', saved: true };
      ui.activeConfiguration = result.activeConfiguration || ui.activeConfiguration;
      if (result.lastLoad) updateLoadStatus(result.lastLoad);
      updateActiveStatus();
      renderState(false);
      updatePresetSelection();
      const issues = [];
      if (!result.alsaApplied) issues.push('ALSA playback control unavailable; volume or mute may not have changed');
      ui.audioApplyError = '';
      refreshDspStatus();
      setSaveStatus(issues.length ? 'Factory defaults restored with warnings' : 'Factory defaults restored', issues.length > 0);
      notify(issues.length ? `Factory defaults restored \u00b7 ${issues.join('; ')}` : 'Factory defaults restored', issues.length > 0);
    }
  } catch (error) {
    notify(error.message, true);
  } finally {
    ui.destructiveBusy = false;
    confirmButton.textContent = confirmLabel;
    confirmButton.disabled = false;
    cancelButton.disabled = false;
    closeButton.disabled = false;
    $('#delete-all-presets').disabled = false;
    $('#copy-production-to-staging').disabled = ui.presetVersion !== 'production';
    $('#restore-defaults').disabled = false;
    closeConfirmAction();
  }
}

async function testSpeaker(driver, button) {
  if (ui.testBusy) return;
  ui.testBusy = true;
  disableAllTests();
  button.disabled = true;
  button.textContent = '\u2026';
  const row = button.closest('.driver-row');
  const durationInput = row.querySelector('[data-driver-duration]');
  const duration = durationInput && durationInput.value ? parseInt(durationInput.value, 10) : 3;
  const minBlockMs = duration * 1000 + 500;
  const startMs = Date.now();
  clearTimeout(ui.saveTimer);
  let savedChanges = false;
  try {
    await request('save', ui.state);
    savedChanges = true;
    const result = await request('test-speaker', { driver, duration });
    notify(`Speaker test completed: ${result.tested}`);
  } catch (error) {
    setSaveStatus(savedChanges ? 'Speaker test failed' : 'Save failed', true);
    notify(error.message, true);
  } finally {
    const elapsed = Date.now() - startMs;
    const remaining = minBlockMs - elapsed;
    if (remaining > 0) await new Promise(r => setTimeout(r, remaining));
    ui.testBusy = false;
    enableAllTests();
  }
}

async function testPgm(button) {
  if (ui.testBusy) return;
  ui.testBusy = true;
  disableAllTests();
  button.disabled = true;
  button.textContent = '\u2026';
  const durationInput = $('#test-pgm-duration');
  const duration = durationInput && durationInput.value ? parseInt(durationInput.value, 10) : 3;
  const minBlockMs = duration * 1000 + 500;
  const startMs = Date.now();
  try {
    const result = await request('test-pgm', { duration });
    notify(`PGM test completed: ${result.tested}`);
  } catch (error) {
    notify(error.message, true);
  } finally {
    const elapsed = Date.now() - startMs;
    const remaining = minBlockMs - elapsed;
    if (remaining > 0) await new Promise(r => setTimeout(r, remaining));
    ui.testBusy = false;
    enableAllTests();
  }
}

function disableAllTests() {
  document.querySelectorAll('[data-test-speaker]').forEach(b => { b.disabled = true; b.textContent = '\u2026'; });
  $('#test-pgm').disabled = true;
  $('#test-pgm').textContent = '\u2026';
}

function enableAllTests() {
  document.querySelectorAll('[data-test-speaker]').forEach(b => { b.disabled = false; b.textContent = 'Test'; });
  $('#test-pgm').disabled = false;
  $('#test-pgm').textContent = 'Test PGM Input';
}

async function quickSavePreset() {
  const version = ui.presetVersion;
  clearTimeout(ui.saveTimer);
  try {
    if (ui.savePromise) await ui.savePromise;
    if (ui.saveRevision !== ui.savedRevision && !(await saveState())) {
      throw new Error(`Could not write current.json before saving ${version}.`);
    }
    const action = version === 'production' ? 'save-production' : 'save-staging';
    const result = await request(action, {
      ...ui.state, presetName: ui.selectedPreset, presetVersion: version,
    });
    if (result.currentJsonWritten !== true) throw new Error('The server did not verify writing current.json.');
    ui.state = result.state;
    ui.currentSource = result.currentSource || { name: ui.selectedPreset, version, saved: true };
    if (result.activeConfiguration) updateActiveStatus(result.activeConfiguration);
    ui.savedRevision = ui.saveRevision;
    setCurrentJsonStatus(true);
    renderState();
    setSaveStatus(`${version === 'production' ? 'Production' : 'Staging'} saved \u00b7 ${ui.selectedPreset}`);
    updateApplyButton();
    notify(`${version === 'production' ? 'Production' : 'Staging'} changes saved to ${ui.selectedPreset}`);
  } catch (error) {
    setSaveStatus(`${version === 'production' ? 'Production' : 'Staging'} save failed`, true);
    notify(error.message, true);
  }
}

async function promotePreset() {
  if (ui.presetVersion !== 'staging') return;
  try {
    clearTimeout(ui.saveTimer);
    if (ui.savePromise) await ui.savePromise;
    if (ui.saveRevision !== ui.savedRevision || !ui.currentSource || !ui.currentSource.saved) {
      throw new Error('Save staging before promoting it to production.');
    }
    const result = await request('preset-promote', { name: ui.selectedPreset, version: ui.presetVersion });
    ui.state = result.state;
    ui.presetVersion = result.presetVersion;
    ui.currentSource = result.currentSource || { name: ui.selectedPreset, version: 'production', saved: true };
    if (result.activeConfiguration) updateActiveStatus(result.activeConfiguration);
    ui.savedRevision = ui.saveRevision;
    if (result.lastLoad) updateLoadStatus(result.lastLoad);
    renderState();
    setSaveStatus(`${ui.selectedPreset} promoted to production`);
    notify(`${ui.selectedPreset} staging promoted to production`);
  } catch (error) { notify(error.message, true); }
}

async function selectPreset(name, version) {
  clearTimeout(ui.saveTimer);
  if (ui.savePromise) await ui.savePromise;
  if (ui.saveRevision !== ui.savedRevision && !(await saveState())) throw new Error('Could not save changes before switching presets.');
  const attemptedAt = new Date().toISOString();
  updateLoadStatus({ snapshot: `${name} ${version[0].toUpperCase()}${version.slice(1)}`, at: attemptedAt });
  const result = await request('preset-select', { name, version });
  ui.selectedPreset = result.selectedPreset;
  ui.presetVersion = result.presetVersion;
  ui.state = result.state;
  ui.currentSource = result.currentSource || { name, version, saved: true };
  if (result.lastLoad) updateLoadStatus(result.lastLoad);
  ui.audioApplyError = '';
  ui.savedRevision = ui.saveRevision;
  renderState();
  refreshDspStatus();
  setSaveStatus(`Loaded ${ui.selectedPreset} ${ui.presetVersion} to current.json`);
  notify(`Loaded ${ui.selectedPreset} ${ui.presetVersion}; apply to engine when ready`);
}
function bindEvents() {
  $('#master-volume').addEventListener('input', (event) => {
    ui.state.volume = Number(event.target.value);
    $('#volume-value').textContent = event.target.value;
  });
  $('#master-volume').addEventListener('change', setMaster);
  $('#mute-button').addEventListener('click', () => {
    ui.state.muted = !ui.state.muted;
    renderState();
    setMaster();
  });
  $('#audio-input').addEventListener('change', (event) => { ui.state.input = event.target.value; queueSave(); });
  $('#sample-rate').addEventListener('change', (event) => {
    ui.state.sampleRate = Number(event.target.value);
    queueSave();
  });
  $('#buffer-size').addEventListener('change', (event) => {
    ui.state.BufferSize = Number(event.target.value);
    queueSave();
  });
  $('#audio-output').addEventListener('change', (event) => { ui.state.output = event.target.value; queueSave(); });
  $('#mic-count').addEventListener('change', (event) => {
    ui.state.micCount = Number(event.target.value);
    queueSave();
    renderMonitorChannel();
  });
  $('#monitor-select').addEventListener('change', (event) => {
    ui.state.monitorChannel = event.target.value;
    queueSave();
  });
  $('#crossover-list').addEventListener('change', (event) => {
    const field = event.target;
    const name = field.dataset.crossoverFrequency || field.dataset.crossoverSlope || field.dataset.crossoverTopology;
    if (!name || !ui.state.crossovers[name]) return;
    if (field.dataset.crossoverFrequency) ui.state.crossovers[name].frequency = Number(field.value);
    if (field.dataset.crossoverSlope) ui.state.crossovers[name].slope = Number(field.value);
    if (field.dataset.crossoverTopology) ui.state.crossovers[name].topology = field.value;
    queueSave();
  });
  document.querySelectorAll('[data-collapse]').forEach((button) => button.addEventListener('click', () => {
    const section = button.dataset.collapse;
    ui.expanded[section] = !ui.expanded[section];
    renderCollapseState();
    if (section === 'peq') drawCurves();
  }));
  ['limiter-enabled', 'limiter-threshold', 'limiter-release'].forEach((id) => {
    $(`#${id}`).addEventListener('change', (event) => {
      if (id === 'limiter-enabled') ui.state.limiter.enabled = event.target.checked;
      if (id === 'limiter-threshold') ui.state.limiter.threshold = Number(event.target.value);
      if (id === 'limiter-release') ui.state.limiter.release = Number(event.target.value);
      queueSave();
    });
  });
  document.querySelectorAll('[data-add-band]').forEach((button) => button.addEventListener('click', () => {
    addBand(button.dataset.addBand === 'woofer' ? 'Woofer' : 'Tweeter');
  }));
  $('#peq-content').addEventListener('click', (event) => {
    const toggle = event.target.closest('[data-band-toggle]');
    const remove = event.target.closest('[data-band-remove]');
    if (toggle) {
      const bands = ui.state.bands[toggle.dataset.bandDriver];
      const band = bands.find((item) => item.id === toggle.dataset.bandToggle);
      if (band) band.enabled = !band.enabled;
      renderBands(); queueSave();
    }
    if (remove) {
      const driverName = remove.dataset.bandDriver;
      ui.state.bands[driverName] = ui.state.bands[driverName].filter((item) => item.id !== remove.dataset.bandRemove);
      renderBands(); queueSave();
    }
  });
  $('#peq-content').addEventListener('input', (event) => {
    const field = event.target;
    const bandId = field.dataset.bandFrequency || field.dataset.bandQ || field.dataset.bandGain;
    if (!bandId) return;
    const band = ui.state.bands[field.dataset.bandDriver].find((item) => item.id === bandId);
    if (!band) return;
    const value = Number(field.value);
    if (!field.value || !Number.isFinite(value)
      || value < Number(field.min) || value > Number(field.max)) return;
    if (field.dataset.bandFrequency) band.frequency = value;
    if (field.dataset.bandQ) band.q = value;
    if (field.dataset.bandGain) band.gain = value;
    queueSave();
  });
  $('#driver-list').addEventListener('change', (event) => {
    const driverIndex = event.target.dataset.driverDelay !== undefined
      ? event.target.dataset.driverDelay
      : event.target.dataset.driverTrim !== undefined
        ? event.target.dataset.driverTrim
        : event.target.dataset.driverPhase;
    const index = Number(driverIndex);
    if (!Number.isInteger(index) || !ui.state.drivers[index]) return;
    if (event.target.dataset.driverDelay !== undefined) ui.state.drivers[index].delayMs = delayToMs(event.target.value);
    if (event.target.dataset.driverTrim !== undefined) ui.state.drivers[index].trimDb = Number(event.target.value);
    if (event.target.dataset.driverPhase !== undefined) ui.state.drivers[index].phaseInverted = event.target.checked;
    queueSave();
  });
  $('#driver-list').addEventListener('click', (event) => {
    const button = event.target.closest('[data-test-speaker]');
    if (button && !button.disabled) testSpeaker(button.dataset.testSpeaker, button);
  });
  $('#test-pgm').addEventListener('click', (event) => testPgm(event.currentTarget));
  document.querySelectorAll('[data-unit]').forEach((button) => button.addEventListener('click', () => {
    ui.delayUnit = button.dataset.unit;
    document.querySelectorAll('[data-unit]').forEach((item) => item.classList.toggle('active', item === button));
    renderDrivers();
  }));
  $('#save-preset').addEventListener('click', openPresetModal);
  $('#save-staging').addEventListener('click', quickSavePreset);
  $('#promote-preset').addEventListener('click', promotePreset);
  $('#close-modal').addEventListener('click', closePresetModal);
  $('#preset-modal').addEventListener('click', (event) => { if (event.target === $('#preset-modal')) closePresetModal(); });
  $('#delete-all-presets').addEventListener('click', (event) => openConfirmAction('preset-delete-all', event.currentTarget));
  $('#copy-production-to-staging').addEventListener('click', (event) => openConfirmAction('preset-copy-production-to-staging', event.currentTarget));
  $('#restore-defaults').addEventListener('click', (event) => openConfirmAction('restore-defaults', event.currentTarget));
  $('#confirm-action').addEventListener('click', performDestructiveAction);
  $('#cancel-confirm-action').addEventListener('click', closeConfirmAction);
  $('#close-confirm-action').addEventListener('click', closeConfirmAction);
  $('#confirm-action-modal').addEventListener('click', (event) => {
    if (event.target === $('#confirm-action-modal') && !ui.destructiveBusy) closeConfirmAction();
  });
  $('#confirm-action-modal').addEventListener('keydown', (event) => {
    if (event.key === 'Escape' && !ui.destructiveBusy) {
      event.preventDefault();
      closeConfirmAction();
    }
    if (event.key === 'Tab') {
      const focusable = [...$('#confirm-action-modal').querySelectorAll('button:not(:disabled)')];
      const first = focusable[0];
      const last = focusable[focusable.length - 1];
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    }
  });
  $('#preset-form').addEventListener('submit', async (event) => {
    event.preventDefault();
    try {
      clearTimeout(ui.saveTimer);
      if (ui.savePromise) await ui.savePromise;
      const result = await request('preset-save', { name: $('#preset-name').value.trim(), state: ui.state });
      ui.presets = result.presets;
      ui.selectedPreset = result.selectedPreset;
      ui.presetVersion = result.presetVersion;
      ui.currentSource = result.currentSource || { name: ui.selectedPreset, version: 'staging', saved: true };
      ui.savedRevision = ui.saveRevision;
      renderPresets();
      renderState(false);
      setSaveStatus(`Staging saved \u00b7 ${ui.selectedPreset}`);
      closePresetModal();
      notify('Preset saved');
    } catch (error) { notify(error.message, true); }
  });
  $('#preset-list').addEventListener('click', async (event) => {
    const select = event.target.closest('[data-preset-select]');
    const remove = event.target.closest('[data-preset-delete]');
    try {
      if (select) {
        await selectPreset(select.dataset.presetSelect, select.dataset.presetVersion);
      }
      if (remove) {
        const deletingSelected = ui.selectedPreset === remove.dataset.presetDelete;
        if (deletingSelected) {
          clearTimeout(ui.saveTimer);
          if (ui.savePromise) await ui.savePromise;
          clearTimeout(ui.saveTimer);
        }
        const result = await request('preset-delete', { name: remove.dataset.presetDelete });
        ui.presets = result.presets;
        ui.selectedPreset = result.selectedPreset;
        ui.presetVersion = result.presetVersion;
        if (result.state) {
          ui.state = result.state;
          ui.savedRevision = ui.saveRevision;
        }
        if (result.state) renderState();
        else renderPresets();
        notify('Preset deleted');
      }
    } catch (error) { notify(error.message, true); }
  });
  $('#measure-button').addEventListener('click', measureRoom);
  $('#apply-button').addEventListener('click', applyToEngine);
  $('#dismiss-measure').addEventListener('click', () => showMeasurement(false));
  window.addEventListener('resize', drawCurve);
}

async function init() {
  try {
    const data = await request('state');
    ui.state = { ...JSON.parse(JSON.stringify(defaults)), ...data.state };



    ui.inputs = data.inputs;
    ui.outputs = data.outputs;
    ui.presets = data.presets;
    ui.selectedPreset = data.selectedPreset;
    ui.presetVersion = data.presetVersion;
    ui.currentSource = data.currentSource || { name: ui.selectedPreset, version: ui.presetVersion, saved: true };
    ui.activeConfiguration = data.activeConfiguration || null;
    ui.lastApplyAttempt = data.lastApplyAttempt || null;
    ui.saveRevision = 0;
    ui.savedRevision = 0;
    updateLoadStatus(data.lastLoad);
    updateActiveStatus();
    ui.measurementReady = data.measurementReady;
    ui.applyReady = data.applyReady;
    await refreshDspStatus();
    renderState();
    bindEvents();
    $('#system-label').textContent = data.alsaReady ? `ALSA connected \u00b7 ${data.alsaControl}` : 'Configuration ready \u00b7 ALSA unavailable';
    $('#system-label').title = data.alsaReady ? '' : data.alsaMessage;
    if (!data.measurementReady) $('#measure-button').title = 'Configure PI_AUDIO_MEASURE_BIN to enable room measurement';
    setSaveStatus('Configuration loaded');
    refreshMetrics();
    setInterval(refreshMetrics, 1000);
    setInterval(refreshDspStatus, 3000);
    setInterval(() => { $('#clock').textContent = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false }); }, 1000);
  } catch (error) {
    $('#system-label').textContent = 'Cannot reach Pi audio API';
    notify(error.message, true);
  }
}

init();
