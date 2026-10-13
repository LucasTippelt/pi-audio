<?php
declare(strict_types=1);
session_start();
if (empty($_SESSION['csrf'])) {
    $_SESSION['csrf'] = bin2hex(random_bytes(24));
}
?>
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="theme-color" content="#f3f1e9">
  <title>Pi Audio / Room tuning</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link href="https://fonts.googleapis.com/css2?family=DM+Mono:wght@400;500&family=Manrope:wght@400;500;600;700;800&display=swap" rel="stylesheet">
  <link rel="stylesheet" href="assets/style.css?v=<?= (int)filemtime(__DIR__ . '/assets/style.css') ?>">
</head>
<body>
  <div class="app-shell">
    <header class="topbar">
      <a class="brand" href="./" aria-label="Pi Audio home">
        <span class="brand-mark" aria-hidden="true"><i></i><i></i><i></i></span>
        <span>pi<span class="brand-slash">/</span>audio</span>
      </a>
      <div class="system-status"><span class="status-light"></span><span id="system-label">Connecting to audio engine</span><span class="mono" id="clock"></span><span class="dsp-status" id="dsp-status" role="status" aria-live="polite"><i></i><span>DSP connection: Checking…</span></span></div>
      <div class="topbar-actions">
        <button class="button button-dark measure-button" id="measure-button" type="button"><span class="measure-icon">◎</span> Measure room</button>
        <div class="apply-row">
          <div class="configuration-status" aria-live="polite">
            <span class="load-status" id="load-status">No snapshot loaded yet</span>
            <span class="load-status" id="active-status">Active: unknown</span>
            <span class="load-status" id="view-status">Viewing: loading</span>
          </div>
          <button class="button button-dark apply-button" id="apply-button" type="button" title="Apply the current.json configuration to the audio engine"><span aria-hidden="true">↻</span> Apply current.json to engine</button>
        </div>
      </div>
    </header>

    <main>
      <section class="overview">
        <div>
          <h1>Make the room <span>disappear.</span></h1>
        </div>
        <div class="master-block">
          <div class="master-top"><label for="master-volume">MASTER</label><button class="mute-button" id="mute-button" type="button" aria-pressed="false">Mute</button></div>
          <div class="volume-readout"><strong id="volume-value">65</strong><span>%</span></div>
          <input id="master-volume" class="range master-range" type="range" min="0" max="100" value="65" aria-label="Master volume">
        </div>
      </section>

      <section class="workspace">
        <section class="response-panel" aria-labelledby="response-title">
          <div class="section-head response-head">
            <div><h2 id="response-title">Frequency response</h2></div>
            <div class="response-meta"><span class="legend"><i class="legend-line measured"></i>MEASURED</span><span class="legend"><i class="legend-line estimate"></i>CROSSOVER + PEQ</span></div>
          </div>
          <div class="chart-wrap"><canvas id="response-chart" aria-label="Left and right frequency response charts from 20 hertz to 20 kilohertz with logarithmic frequency ticks and 3 decibel grid increments" role="img"></canvas></div>
          <div class="chart-footer"><span id="curve-caption"></span></div>
        </section>

        <aside class="side-rail">
          <section class="control-section input-section">
            <div class="section-head"><div><h2>Audio input</h2></div></div>
            <label class="field-label" for="audio-input">INPUT (*ALSA CAPTURE DEVICE, STREAMLINE SELECTION IN FINAL PHASE OF DEVELOPMENT*)</label>
            <select class="select-control" id="audio-input"><option value="default">Loading devices…</option></select>
            <div class="input-options">
              <div><label class="field-label" for="sample-rate">SAMPLE RATE</label>
                <select class="select-control" id="sample-rate" disabled>
                  <option value="" selected>Loading…</option>
                  <option value="44100">44.1 kHz</option>
                  <option value="48000">48 kHz</option>
                  <option value="88200">88.2 kHz</option>
                  <option value="96000">96 kHz</option>
                  <option value="176400">176.4 kHz</option>
                  <option value="192000">192 kHz</option>
                </select>
              </div>
              <div><label class="field-label" for="buffer-size">BUFFER SIZE</label>
                <select class="select-control" id="buffer-size" disabled>
                  <option value="" selected>Loading…</option>
                  <option value="64">64 samples</option>
                  <option value="128">128 samples</option>
                  <option value="256">256 samples</option>
                  <option value="512">512 samples</option>
                  <option value="1024">1,024 samples</option>
                  <option value="2048">2,048 samples</option>
                  <option value="4096">4,096 samples</option>
                </select>
              </div>
            </div>
            <p class="field-note" id="input-note">Selecting an input, sample rate, or buffer size applies the choice to the audio engine.</p>
            <button class="test-pgm" type="button" id="test-pgm" aria-label="Test PGM Input" title="Play stereo whitenoise on Program output">Test PGM Input</button>
            <label class="field-label" for="audio-output">ALSA PLAYBACK DEVICE</label>
            <select class="select-control" id="audio-output"><option value="default">Loading devices…</option></select>
            <p class="field-note" id="output-note">For debugging only; remove after development.</p>
          </section>

          <section class="control-section crossover-section">
            <div class="section-head">
              <div><h2>Crossovers</h2></div>
              <div class="segmented" role="group" aria-label="Crossover channel"><button type="button" data-crossover-side="left" class="active" aria-pressed="true">Left</button><button type="button" data-crossover-side="right" aria-pressed="false">Right</button></div>
            </div>
            <div class="crossover-list" id="crossover-list"></div>
          </section>
        </aside>
      </section>

      <section class="control-section eq-section">
        <div class="section-head">
          <div><h2>Parametric EQ</h2></div>
          <div class="section-head-actions">
            <div class="segmented" role="group" aria-label="Parametric EQ channel"><button type="button" data-peq-side="left" class="active" aria-pressed="true">Left</button><button type="button" data-peq-side="right" aria-pressed="false">Right</button></div>
            <button class="collapse-button" type="button" data-collapse="peq" aria-controls="peq-content" aria-expanded="true" aria-label="Minimize parametric EQ controls">-</button>
          </div>
        </div>
        <div id="peq-content">
          <div class="peq-drivers">
            <section class="peq-driver" aria-labelledby="woofer-peq-title">
              <div class="peq-driver-head"><h3 id="woofer-peq-title">Woofer <span class="count" id="woofer-band-count">00</span></h3><button class="add-band-link" type="button" data-add-band="woofer" aria-label="Add Woofer PEQ band">+ Add band</button></div>
              <div class="band-list" id="woofer-band-list"></div>
            </section>
            <section class="peq-driver" aria-labelledby="tweeter-peq-title">
              <div class="peq-driver-head"><h3 id="tweeter-peq-title">Tweeter <span class="count" id="tweeter-band-count">00</span></h3><button class="add-band-link" type="button" data-add-band="tweeter" aria-label="Add Tweeter PEQ band">+ Add band</button></div>
              <div class="band-list" id="tweeter-band-list"></div>
            </section>
          </div>
        </div>
      </section>

      <section class="lower-grid">
        <section class="control-section driver-section">
          <div class="section-head"><div><h2>Drivers</h2></div></div>
          <div class="driver-unit"><span>DELAY UNIT</span><div class="segmented" role="group" aria-label="Delay unit"><button type="button" data-unit="ms" class="active">ms</button><button type="button" data-unit="cm">cm</button></div></div>
          <div class="driver-table-head"><span>OUTPUT</span><span>DELAY</span><span>TRIM</span><span>PHASE INV</span><span>TEST</span></div>
          <div id="driver-list" class="driver-list"></div>
        </section>

        <section class="control-section limiter-section">
          <div class="section-head"><div><h2>Peak limiter</h2></div><label class="switch"><input id="limiter-enabled" type="checkbox" aria-label="Enable limiter"><span></span></label></div>
          <div class="limiter-fields"><label><span class="field-label">THRESHOLD</span><div class="number-input"><input id="limiter-threshold" type="number" min="-24" max="0" step="0.5"><span>dBFS</span></div></label><label><span class="field-label">RELEASE</span><div class="number-input"><input id="limiter-release" type="number" min="10" max="2000" step="10"><span>ms</span></div></label></div>
          <p class="field-note">Limiter settings are written to the active DSP configuration.</p>
        </section>

        <section class="control-section metrics-section">
          <div class="section-head"><div><h2>Performance</h2></div><span class="live-mark" id="metrics-status"><i></i>LIVE</span></div>
          <div class="metric-row"><span>CPU LOAD</span><strong id="metric-cpu">--<small>%</small></strong><div class="metric-bar"><i id="cpu-bar"></i></div></div>
          <div class="metric-row"><span>CORE TEMP</span><strong id="metric-temp">--<small>°C</small></strong><div class="temp-mark" id="temp-mark"></div></div>
          <div class="metric-row"><span>AUDIO XRUNS</span><strong id="metric-xruns">--<small> count</small></strong><span class="metric-foot" id="xrun-foot">since engine start</span></div>
        </section>
      </section>

      <section class="preset-bar">
        <div class="preset-title"><strong>Presets</strong><span id="preset-mode">Default · staging</span></div>
        <div class="preset-list" id="preset-list"></div>
        <div class="preset-actions">
          <button class="button button-outline" id="save-staging" type="button"><span>↻</span> Save staging</button>
          <button class="button button-outline" id="copy-production-to-staging" type="button" disabled>Copy production to staging</button>
          <button class="button button-outline" id="promote-preset" type="button" disabled>Promote to production</button>
          <button class="button button-outline" id="save-preset" type="button"><span>＋</span> Save as new preset</button>
          <button class="button button-outline button-danger" id="delete-all-presets" type="button">Delete all except Default</button>
          <button class="button button-outline button-danger" id="restore-defaults" type="button">Restore to Factory Default</button>
        </div>
      </section>
    </main>

    <footer class="footer"><span>PI / AUDIO CONTROL</span><span id="current-json-status" class="current-json-status" aria-live="polite">current.json: not checked</span><span id="save-status">Changes save to the active configuration</span><span>LOCAL NETWORK CONSOLE</span></footer>
  </div>

  <div class="toast" id="toast" role="status" aria-live="polite"></div>
  <div class="modal-backdrop" id="preset-modal" hidden>
    <form class="modal" id="preset-form"><button type="button" class="modal-close" id="close-modal" aria-label="Close">×</button><h2>Save as new preset</h2><label class="field-label" for="preset-name">PRESET NAME</label><input class="text-input" id="preset-name" maxlength="32" required placeholder="e.g. Late Night"><div class="modal-actions"><button class="button button-dark" type="submit">Create preset</button></div></form>
  </div>
  <div class="modal-backdrop" id="confirm-action-modal" hidden>
    <section class="modal" role="alertdialog" aria-modal="true" aria-labelledby="confirm-action-title" aria-describedby="confirm-action-description">
      <button type="button" class="modal-close" id="close-confirm-action" aria-label="Close">×</button>
      <h2 id="confirm-action-title"></h2>
      <p id="confirm-action-description"></p>
      <div class="modal-actions confirm-actions">
        <button class="button button-outline" id="cancel-confirm-action" type="button">Cancel</button>
        <button class="button button-dark button-danger" id="confirm-action" type="button">Confirm</button>
      </div>
    </section>
  </div>
  <div class="measure-overlay" id="measure-overlay" hidden><div class="measure-dialog"><div class="measure-pulse"><span></span></div><h2 id="measure-title">Listening to the room</h2><p id="measure-detail">Follow the measurement system's prompts. This may take a minute.</p><div class="measure-progress"><i></i></div><button class="text-button" id="dismiss-measure" type="button">Dismiss</button></div></div>
  <script src="assets/app.js?v=<?= (int)filemtime(__DIR__ . '/assets/app.js') ?>" defer></script>
<a href="debug.php" style="position:fixed;bottom:8px;right:8px;font-size:11px;opacity:0.3;text-decoration:none;color:#888;" title="Debug page">⚙ debug</a>
</body>
</html>