<?php
declare(strict_types=1);
session_start();
?>
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Pi Audio / Debug</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: 'DM Mono', monospace; background: #1a1a2e; color: #e0e0e0; padding: 2rem; line-height: 1.6; }
    h1 { color: #00ff88; margin-bottom: 1.5rem; font-size: 1.5rem; }
    h2 { color: #00ccff; margin: 1.5rem 0 0.5rem; font-size: 1.1rem; border-bottom: 1px solid #333; padding-bottom: 0.3rem; }
    .section { margin-bottom: 2rem; }
    table { width: 100%; border-collapse: collapse; margin: 0.5rem 0; }
    th, td { text-align: left; padding: 0.4rem 0.8rem; border-bottom: 1px solid #2a2a3e; }
    th { color: #888; width: 40%; }
    td { color: #e0e0e0; }
    .ok { color: #00ff88; }
    .warn { color: #ffaa00; }
    .err { color: #ff4444; }
    pre { background: #0d0d1a; padding: 1rem; border-radius: 4px; overflow-x: auto; font-size: 0.85rem; white-space: pre-wrap; word-break: break-all; }
    a { color: #00ccff; }
    .badge { display: inline-block; padding: 0.1rem 0.5rem; border-radius: 3px; font-size: 0.8rem; }
    .badge-ok { background: #003322; color: #00ff88; }
    .badge-err { background: #330011; color: #ff4444; }
    .badge-warn { background: #332200; color: #ffaa00; }
    #refresh-bar { position: fixed; top: 0; left: 0; height: 2px; background: #00ff88; transition: width linear; }
    #refresh-status { position: fixed; top: 0.5rem; right: 1rem; font-size: 0.75rem; color: #555; }
  </style>
</head>
<body>
  <h1>Pi Audio Debug</h1>
  <p><a href="./">← Back to main page</a></p>

  <?php
  function row(string $label, mixed $value, string $cls = ''): void {
      echo "<tr><th>$label</th><td class=\"$cls\">" . htmlspecialchars((string)$value) . "</td></tr>\n";
  }
  function badge(string $label, bool $ok): void {
      echo "<span class=\"badge " . ($ok ? 'badge-ok' : 'badge-err') . "\">$label</span>";
  }
  function sectionHeading(string $title): void {
      echo "<h2>$title</h2>\n";
  }
  function getBinaryStatus(string $name, string $logPath): array {
      $result = ['running' => false, 'lastRun' => null, 'lastStatus' => 'unknown', 'lastLine' => ''];
      $base = basename($name);
      $pids = @shell_exec("pgrep -f " . escapeshellarg($name) . " 2>/dev/null");
      if ($pids && trim($pids) !== '') {
          $result['running'] = true;
          $result['pids'] = array_map('trim', explode("\n", trim($pids)));
      }
      if (is_file($logPath)) {
          $lines = @file($logPath);
          if ($lines) {
              $lastLine = trim(end($lines));
              if ($lastLine !== '') {
                  $result['lastLine'] = $lastLine;
                  if (preg_match('/\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\]/', $lastLine, $m)) {
                      $result['lastRun'] = $m[1];
                  }
                  if (stripos($lastLine, 'ERROR') !== false || stripos($lastLine, 'FAILED') !== false) {
                      $result['lastStatus'] = 'error';
                  } elseif (stripos($lastLine, 'INFO') !== false && (stripos($lastLine, 'success') !== false || stripos($lastLine, 'applied') !== false || stripos($lastLine, 'written') !== false || stripos($lastLine, 'initialized') !== false)) {
                      $result['lastStatus'] = 'success';
                  } elseif (stripos($lastLine, 'INFO') !== false) {
                      $result['lastStatus'] = 'info';
                  }
              }
          }
      }
      return $result;
  }

  // ---- ENVIRONMENT ----
  sectionHeading('Environment Variables');
  echo "<table>\n";
  $envVars = [
      'PI_AUDIO_STATUS_BIN',
      'PI_AUDIO_APPLY_BIN',
      'PI_AUDIO_MEASURE_BIN',
      'PI_AUDIO_CONFIG_DIR',
      'PI_AUDIO_PRESETS_DIR',
      'PI_AUDIO_CARD',
      'PI_AUDIO_CONTROL',
      'PI_AUDIO_XRUN_FILE',
  ];
  foreach ($envVars as $v) {
      $val = getenv($v);
      $cls = $val ? 'ok' : 'warn';
      row($v, $val !== false && $val !== '' ? $val : '(not set)', $cls);
  }
  echo "</table>\n";

  // ---- VERSIONS ----
  sectionHeading('Build Versions');
  echo "<table>\n";
  $versionFile = __DIR__ . '/VERSION.json';
  $webVersion = '(not generated)';
  $webBuildTime = '-';
  if (is_file($versionFile)) {
      $vdata = json_decode(file_get_contents($versionFile), true);
      if (is_array($vdata)) {
          $webVersion = $vdata['version'] ?? '?';
          $webBuildTime = $vdata['buildTime'] ?? '-';
      }
  }
  row('Web / WS Server', "$webVersion ($webBuildTime)", is_file($versionFile) ? 'ok' : 'warn');

  /* Engine version from engine-state.json */
  $engineStatePath = '/run/pi-audio/engine-state.json';
  $engineVersion = '(not running)';
  if (is_file($engineStatePath)) {
      $es = json_decode(file_get_contents($engineStatePath), true);
      if (is_array($es) && isset($es['version'])) {
          $engineVersion = $es['version'];
      }
  }
  row('Audio Engine', $engineVersion, $engineVersion !== '(not running)' ? 'ok' : 'warn');

  /* Check if versions match */
  if ($webVersion !== '(not generated)' && $engineVersion !== '(not running)') {
      $match = ($webVersion === $engineVersion);
      row('Version Match', $match ? 'Yes' : 'MISMATCH', $match ? 'ok' : 'err');
  }
  echo "</table>\n";

  // ---- PHP INFO ----
  sectionHeading('PHP / Server');
  echo "<table>\n";
  row('PHP Version', phpversion());
  row('Server Software', $_SERVER['SERVER_SOFTWARE'] ?? 'unknown');
  row('Document Root', $_SERVER['DOCUMENT_ROOT'] ?? 'unknown');
  row('PHP User', function_exists('posix_getpwuid') ? posix_getpwuid(posix_geteuid())['name'] : php_uname('u'));
  row('Real UID/GID', function_exists('posix_getuid') ? posix_getuid() . '/' . posix_getgid() : 'N/A');
  row('Effective UID/GID', function_exists('posix_geteuid') ? posix_geteuid() . '/' . posix_getegid() : 'N/A');
  echo "</table>\n";

  // ---- CONFIG FILES ----
  sectionHeading('Configuration Files');
  $configDir = getenv('PI_AUDIO_CONFIG_DIR') ?: '/etc/pi-audio/config';
  $presetsDir = getenv('PI_AUDIO_PRESETS_DIR') ?: '/etc/pi-audio/presets';
  $currentPath = $configDir . '/current.json';

  echo "<table>\n";
  row('Config Dir', $configDir, is_dir($configDir) ? 'ok' : 'err');
  row('Config Writable', is_writable($configDir) ? 'Yes' : 'No', is_writable($configDir) ? 'ok' : 'err');
  row('Presets Dir', $presetsDir, is_dir($presetsDir) ? 'ok' : 'err');
  row('Presets Writable', is_writable($presetsDir) ? 'Yes' : 'No', is_writable($presetsDir) ? 'ok' : 'err');
  row('Current Config', $currentPath, is_file($currentPath) ? 'ok' : 'err');

  $statusPath = '/run/pi-audio/status';
  row('Status File', $statusPath, is_file($statusPath) ? 'ok' : 'warn');
  echo "</table>\n";

  // ---- CURRENT CONFIG ----
  sectionHeading('Current Configuration (current.json)');
  if (is_file($currentPath)) {
      $raw = file_get_contents($currentPath);
      $config = json_decode($raw, true);
      if (is_array($config)) {
          echo "<table>\n";
          row('Input Device', $config['input'] ?? '—');
          row('Output Device', $config['output'] ?? '—');
          row('Sample Rate', ($config['sampleRate'] ?? '—') . ' Hz');
          row('Buffer Size', ($config['BufferSize'] ?? '—') . ' frames');
          row('Volume', ($config['volume'] ?? '—') . '%');
          row('Muted', ($config['muted'] ?? false) ? 'Yes' : 'No');

          if (isset($config['limiter']) && is_array($config['limiter'])) {
              row('Limiter', 'enabled: ' . ($config['limiter']['enabled'] ? 'Yes' : 'No') .
                  ', threshold: ' . ($config['limiter']['threshold'] ?? '—') . ' dB' .
                  ', release: ' . ($config['limiter']['release'] ?? '—') . ' ms');
          }

          if (isset($config['drivers']) && is_array($config['drivers'])) {
              $drivers = '';
              foreach ($config['drivers'] as $d) {
                  $drivers .= ($d['name'] ?? '?') . ' (delay: ' . ($d['delayMs'] ?? 0) . 'ms, trim: ' . ($d['trimDb'] ?? 0) . 'dB)' . "\n";
              }
              row('Drivers', '<pre>' . htmlspecialchars(trim($drivers)) . '</pre>');
          }

          if (isset($config['bands']) && is_array($config['bands'])) {
              $bands = '';
              foreach ($config['bands'] as $name => $b) {
                  $bands .= "$name: " . count((array)$b) . " bands\n";
              }
              row('Bands', '<pre>' . htmlspecialchars(trim($bands)) . '</pre>');
          }
          echo "</table>\n";
      } else {
          echo "<p class='err'>Invalid JSON in current.json</p>\n";
      }
  } else {
      echo "<p class='err'>current.json not found</p>\n";
  }

  // ---- ADAPTERS ----
  sectionHeading('Adapter Binaries');
  echo "<table>\n";
  $adapters = [
      'Status' => getenv('PI_AUDIO_STATUS_BIN'),
      'Apply' => getenv('PI_AUDIO_APPLY_BIN'),
      'Measure' => getenv('PI_AUDIO_MEASURE_BIN'),
  ];
  foreach ($adapters as $name => $path) {
      if (!$path) {
          row($name, '<em>not configured</em>', 'warn');
      } else {
          $exists = is_file($path);
          $exec = is_executable($path);
          $cls = $exec ? 'ok' : 'err';
          row($name . ' (' . basename($path) . ')',
              ($exists ? 'exists' : 'MISSING') . ' | ' . ($exec ? 'executable' : 'NOT executable'),
              $cls);
      }
  }
   echo "</table>\n";

  // ---- TEST SPEAKER BINARIES ----
  sectionHeading('Test Speaker Binaries');
  echo "<table>\n";
  $testDrivers = [
      'Woofer_L' => getenv('PI_AUDIO_TEST_BIN_WOOFER_L'),
      'Tweeter_L' => getenv('PI_AUDIO_TEST_BIN_TWEETER_L'),
      'Woofer_R' => getenv('PI_AUDIO_TEST_BIN_WOOFER_R'),
      'Tweeter_R' => getenv('PI_AUDIO_TEST_BIN_TWEETER_R'),
  ];
  foreach ($testDrivers as $driver => $path) {
      if (!$path) {
          row($driver, '<em>not configured</em>', 'warn');
      } else {
          $exists = is_file($path);
          $exec = is_executable($path);
          $cls = $exec ? 'ok' : 'err';
          row($driver . ' (' . basename($path) . ')',
              ($exists ? 'exists' : 'MISSING') . ' | ' . ($exec ? 'executable' : 'NOT executable'),
              $cls);
      }
  }
   echo "</table>\n";

  // ---- BINARY RUNTIME STATUS ----
  sectionHeading('Binary Runtime Status');
  echo "<table>\n";
  echo "<tr><th>Binary</th><th>Running</th><th>Last Run</th><th>Status</th><th>Last Log Line</th></tr>\n";
  $binaries = [
      'Status' => ['bin' => getenv('PI_AUDIO_STATUS_BIN'), 'log' => '/var/log/pi-audio/status.log'],
      'Apply' => ['bin' => getenv('PI_AUDIO_APPLY_BIN'), 'log' => '/var/log/pi-audio/apply.log'],
      'Measure' => ['bin' => getenv('PI_AUDIO_MEASURE_BIN'), 'log' => '/var/log/pi-audio/measure.log'],
      'Test' => ['bin' => '/usr/local/bin/pi-audio-test', 'log' => '/var/log/pi-audio/test.log'],
  ];
  foreach ($binaries as $label => $info) {
      $bin = $info['bin'] ?: '/usr/local/bin/pi-audio-' . strtolower($label);
      $log = $info['log'];
      $st = getBinaryStatus($bin, $log);
      $runCls = $st['running'] ? 'ok' : '';
      $runBadge = $st['running']
          ? '<span class="badge badge-ok">PID ' . implode(', ', $st['pids']) . '</span>'
          : '<span class="badge badge-err">stopped</span>';
      $lastRun = $st['lastRun'] ? $st['lastRun'] : 'never';
      $statusCls = $st['lastStatus'] === 'error' ? 'err' : ($st['lastStatus'] === 'success' ? 'ok' : 'warn');
      $statusLabel = ucfirst($st['lastStatus']);
      $lastLine = htmlspecialchars(substr($st['lastLine'], -80));
      echo "<tr>";
      echo "<th>$label</th>";
      echo "<td class=\"$runCls\">$runBadge</td>";
      echo "<td>$lastRun</td>";
      echo "<td class=\"$statusCls\">$statusLabel</td>";
      echo "<td style=\"font-size:0.75rem;color:#888;\">$lastLine</td>";
      echo "</tr>\n";
  }
  echo "</table>\n";

  // ---- MONITOR RING BUFFER ----
  sectionHeading('Monitor Ring Buffer');
  echo "<table>\n";
  $monitorPath = '/run/pi-audio/audio-monitor.raw';
  if (is_file($monitorPath)) {
      $mSize = filesize($monitorPath);
      $mPerms = substr(sprintf('%o', fileperms($monitorPath)), -4);
      $mOwner = function_exists('posix_getpwuid') ? posix_getpwuid(fileowner($monitorPath))['name'] : fileowner($monitorPath);
      $mGroup = function_exists('posix_getgrgid') ? posix_getgrgid(filegroup($monitorPath))['name'] : filegroup($monitorPath);
      $mReadable = is_readable($monitorPath) ? 'Yes' : 'No';
      row('Path', $monitorPath, 'ok');
      row('Size', number_format($mSize) . ' bytes (' . round($mSize / 1024 / 1024, 2) . ' MB)');
      row('Permissions', "$mPerms $mOwner:$mGroup", $mReadable ? 'ok' : 'err');
      row('Readable by www-data', $mReadable, $mReadable ? 'ok' : 'err');

      /* Read header */
      $mfp = fopen($monitorPath, 'rb');
      if ($mfp) {
          fseek($mfp, 0);
          $mhdr = fread($mfp, 64);
          fclose($mfp);
          if ($mhdr && strlen($mhdr) >= 24 && substr($mhdr, 0, 4) === 'PIAM') {
              $mWriteL = unpack('V', substr($mhdr, 6, 4))[1];
              $mWriteH = unpack('V', substr($mhdr, 10, 4))[1];
              $mWritePos = ($mWriteH << 32) | $mWriteL;
              $mSr = unpack('V', substr($mhdr, 14, 4))[1];
              $mCh = unpack('V', substr($mhdr, 18, 4))[1];
              $mSeq = unpack('V', substr($mhdr, 20, 4))[1];
              row('Magic', substr($mhdr, 0, 4), 'ok');
              row('Sample Rate', $mSr . ' Hz');
              row('Channels', $mCh);
              row('Write Position', number_format($mWritePos) . ' bytes');
              row('Sequence', $mSeq);
              $utilPct = round(($mWritePos / $mSize) * 100, 1);
              row('Buffer Usage', round($mWritePos / 1024 / 1024, 2) . ' MB (' . $utilPct . '%)');
          } else {
              row('Header', 'Invalid magic bytes', 'err');
          }
      }
  } else {
      row('Path', $monitorPath, 'warn');
      row('Status', 'Not found — engine may not have started yet', 'warn');
  }
  echo "</table>\n";

  // ---- ALSA ----
  sectionHeading('ALSA Devices');
  echo "<table>\n";
  $cards = @file_get_contents('/proc/asound/cards');
  row('Sound Cards', $cards !== false ? htmlspecialchars(trim($cards)) : 'N/A');

  $devices = @file_get_contents('/proc/asound/devices');
  row('ALSA Devices', $devices !== false ? '<pre>' . htmlspecialchars(trim($devices)) . '</pre>' : 'N/A');

  // Check amixer
  $amixerTest = shell_exec('amixer scontrols 2>&1');
  row('amixer scontrols', $amixerTest ? '<pre>' . htmlspecialchars(trim($amixerTest)) . '</pre>' : '<em>failed</em>');
  echo "</table>\n";

  // ---- DSP STATUS ----
  sectionHeading('DSP Connection Status');
  $statusBin = getenv('PI_AUDIO_STATUS_BIN');
  if ($statusBin && is_executable($statusBin)) {
      $output = shell_exec($statusBin . ' 2>&1');
      $status = json_decode($output, true);
      if (is_array($status)) {
          echo "<table>\n";
          $connected = $status['connected'] ?? false;
          row('Connected', $connected ? 'Yes' : 'No', $connected ? 'ok' : 'warn');
          row('Sample Rate', ($status['sampleRate'] ?? '—') . ' Hz');
          row('Buffer Size', ($status['BufferSize'] ?? '—') . ' frames');
          if (isset($status['message'])) row('Message', $status['message']);
          echo "</table>\n";
      } else {
          echo "<p class='err'>Invalid JSON from status adapter: <pre>" . htmlspecialchars((string)$output) . "</pre></p>\n";
      }
  } else {
      echo "<p class='warn'>Status adapter not available</p>\n";
  }

  // ---- ENGINE STATE vs CONFIG ----
  sectionHeading('Engine State vs Config (Apply Verification)');
  $statePath = '/run/pi-audio/engine-state.json';
  if (is_file($statePath)) {
      $engineState = json_decode(file_get_contents($statePath), true);
      if (is_array($engineState) && is_array($config ?? null)) {
          $fields = [
              'input' => 'Input Device',
              'output' => 'Output Device',
              'sampleRate' => 'Sample Rate',
              'BufferSize' => 'Buffer Size',
              'volume' => 'Volume',
              'muted' => 'Muted',
          ];
          echo "<table>\n";
          echo "<tr><th>Parameter</th><th>Config File</th><th>Engine State</th><th>Match</th></tr>\n";
          foreach ($fields as $key => $label) {
              $cfgVal = $config[$key] ?? '—';
              $engVal = $engineState[$key] ?? '—';
              $cfgStr = is_bool($cfgVal) ? ($cfgVal ? 'true' : 'false') : (is_array($cfgVal) ? 'array' : (string)$cfgVal);
              $engStr = is_bool($engVal) ? ($engVal ? 'true' : 'false') : (is_array($engVal) ? 'array' : (string)$engVal);
              $match = ($cfgStr === $engStr);
              $matchCls = $match ? 'ok' : 'err';
              $matchIcon = $match ? '✓' : '✗';
              echo "<tr>";
              echo "<th>$label</th>";
              echo "<td>$cfgStr</td>";
              echo "<td>$engStr</td>";
              echo "<td class=\"$matchCls\">$matchIcon</td>";
              echo "</tr>\n";
          }
          echo "</table>\n";
      }
  } else {
      echo "<p class='warn'>Engine state file not found at $statePath — engine may not be writing state, or engine-state.json was not created after the latest build.</p>\n";
  }

  // ---- SERVICE STATUS ----
  sectionHeading('Systemd Service');
  $svcStatus = shell_exec('systemctl is-active pi-audio-engine 2>&1');
  row('pi-audio-engine', trim((string)$svcStatus), trim((string)$svcStatus) === 'active' ? 'ok' : 'err');

  $svcCheck = shell_exec('systemctl status pi-audio-engine 2>&1');
  row('Service Status', '<pre>' . htmlspecialchars(trim((string)$svcCheck)) . '</pre>');

  // ---- PRESETS ----
  sectionHeading('Presets');
  if (is_dir($presetsDir)) {
      $presets = glob($presetsDir . '/*.json');
      echo "<table>\n";
      row('Preset Count', count($presets) . ' files');
      foreach ($presets as $p) {
          $name = basename($p, '.json');
          $size = filesize($p);
          $mod = date('Y-m-d H:i:s', filemtime($p));
          row($name, "$size bytes | modified $mod");
      }
      echo "</table>\n";
  } else {
      echo "<p class='err'>Presets directory not found: $presetsDir</p>\n";
  }

  // ---- SESSION ----
  sectionHeading('PHP Session');
  echo "<table>\n";
  row('CSRF Token', substr($_SESSION['csrf'] ?? 'none', 0, 16) . '…');
  row('Selected Preset', $_SESSION['selectedPreset'] ?? 'none');
  row('Preset Version', $_SESSION['presetVersion'] ?? 'none');
  if (isset($_SESSION['currentSource']) && is_array($_SESSION['currentSource'])) {
      row('Current Source', ($_SESSION['currentSource']['name'] ?? '?') . ' ' . ($_SESSION['currentSource']['version'] ?? '?') .
          ' (saved: ' . ($_SESSION['currentSource']['saved'] ? 'Yes' : 'No') . ')');
  }
  echo "</table>\n";

  // ---- PERMISSIONS ----
  sectionHeading('File Permissions');
  echo "<table>\n";
  $permPaths = [
      $configDir,
      $currentPath,
      $presetsDir,
      '/var/log/pi-audio',
      '/run/pi-audio',
      '/usr/local/bin/pi-audio-engine',
      '/usr/local/bin/pi-audio-status',
      '/usr/local/bin/pi-audio-apply',
  ];
  foreach ($permPaths as $p) {
      if (file_exists($p)) {
          $perms = substr(sprintf('%o', fileperms($p)), -4);
          $owner = function_exists('posix_getpwuid') ? posix_getpwuid(fileowner($p))['name'] : fileowner($p);
          $group = function_exists('posix_getgrgid') ? posix_getgrgid(filegroup($p))['name'] : filegroup($p);
          $writable = is_writable($p) ? 'w' : '-';
          row("$p", "$perms $owner:$group [$writable]");
      } else {
          row("$p", '<em>not found</em>', 'warn');
      }
  }
  echo "</table>\n";
  ?>

  <h2>Raw current.json</h2>
  <?php
  if (is_file($currentPath)) {
      echo "<pre>" . htmlspecialchars(file_get_contents($currentPath)) . "</pre>\n";
  } else {
      echo "<p class='err'>File not found</p>\n";
  }
  ?>
  <div id="refresh-bar"></div>
  <div id="refresh-status">Auto-refresh: 5s</div>
  <script>
    (function() {
      const interval = 5000;
      let start = Date.now();
      const bar = document.getElementById('refresh-bar');
      const status = document.getElementById('refresh-status');
      function tick() {
        const elapsed = Date.now() - start;
        const pct = Math.min(100, (elapsed / interval) * 100);
        bar.style.width = pct + '%';
        const remaining = Math.max(0, Math.ceil((interval - elapsed) / 1000));
        status.textContent = 'Refresh in ' + remaining + 's';
        if (elapsed >= interval) {
          location.reload();
        } else {
          requestAnimationFrame(tick);
        }
      }
      requestAnimationFrame(tick);
    })();
  </script>
</body>
</html>
