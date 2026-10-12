<?php
declare(strict_types=1);
session_start();
header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');

const DEFAULT_STATE = [
    'input' => 'default', 'output' => 'default', 'sampleRate' => 48000, 'BufferSize' => 512, 'volume' => 65, 'muted' => false,
    'crossovers' => [
        'Woofer_L' => ['frequency' => 80, 'topology' => 'Linkwitz-Riley', 'slope' => 24],
        'Tweeter_L' => ['frequency' => 80, 'topology' => 'Linkwitz-Riley', 'slope' => 24],
        'Woofer_R' => ['frequency' => 80, 'topology' => 'Linkwitz-Riley', 'slope' => 24],
        'Tweeter_R' => ['frequency' => 80, 'topology' => 'Linkwitz-Riley', 'slope' => 24],
    ],
    'bands' => [
        'Woofer_L' => [], 'Tweeter_L' => [], 'Woofer_R' => [], 'Tweeter_R' => [],
    ],
    'drivers' => [
        ['name' => 'Woofer_L', 'delayMs' => 0, 'trimDb' => 0, 'phaseInverted' => false],
        ['name' => 'Tweeter_L', 'delayMs' => 0, 'trimDb' => 0, 'phaseInverted' => false],
        ['name' => 'Woofer_R', 'delayMs' => 0, 'trimDb' => 0, 'phaseInverted' => false],
        ['name' => 'Tweeter_R', 'delayMs' => 0, 'trimDb' => 0, 'phaseInverted' => false],
    ],
    'limiter' => ['enabled' => true, 'threshold' => -1, 'release' => 80],
    'measurement' => null,
];

function configDir(): string { return getenv('PI_AUDIO_CONFIG_DIR') ?: '/etc/pi-audio/config'; }
function presetsDir(): string { return getenv('PI_AUDIO_PRESETS_DIR') ?: '/etc/pi-audio/presets'; }
function currentPath(): string { return configDir() . '/current.json'; }
function mixerStatus(): array {
    $configuredCard = getenv('PI_AUDIO_CARD');
    if ($configuredCard !== false && $configuredCard !== '') {
        $cards = [$configuredCard];
    } else {
        $cards = [];
        $rawCards = @file_get_contents('/proc/asound/cards');
        if ($rawCards !== false && preg_match_all('/^\s*(\d+)\s+\[/m', $rawCards, $matches)) {
            $cards = $matches[1];
        }
        if (!$cards) $cards = ['0'];
    }

    $configuredControl = getenv('PI_AUDIO_CONTROL');
    $devices = array_map(static fn($card) => ['card' => (string)$card, 'label' => 'card ' . $card], $cards);
    if ($configuredCard === false || $configuredCard === '') $devices[] = ['card' => null, 'label' => 'default device'];
    $errors = [];
    foreach ($devices as $device) {
        $command = ['amixer'];
        if ($device['card'] !== null) array_push($command, '-c', $device['card']);
        $controls = runCommand(array_merge($command, ['scontrols']));
        if (!$controls['ok']) {
            $error = $controls['error'] ?: $controls['output'];
            if ($error !== '') $errors[] = $device['label'] . ': ' . $error;
            continue;
        }
        preg_match_all("/Simple mixer control '([^']+)'/", $controls['output'], $matches);
        $available = array_values(array_unique($matches[1] ?? []));
        if ($configuredControl !== false && $configuredControl !== '') {
            $available = array_values(array_filter($available, fn($control) => $control === $configuredControl));
        } else {
            usort($available, static function (string $left, string $right): int {
                $priority = ['Master' => 0, 'PCM' => 1, 'Speaker' => 2, 'Headphone' => 3];
                return ($priority[$left] ?? 4) <=> ($priority[$right] ?? 4);
            });
        }

        foreach ($available as $control) {
            $details = runCommand(array_merge($command, ['sget', $control]));
            if (!$details['ok']) continue;
            if (($configuredControl === false || $configuredControl === '') &&
                !preg_match('/Playback channels:|\b(?:pvolume|pswitch)\b/i', $details['output'])) continue;
            return ['target' => ['card' => $device['card'], 'control' => $control], 'message' => ''];
        }
    }

    $errorText = implode("\n", $errors);
    if (str_contains($errorText, 'Command unavailable')) {
        $message = 'amixer is unavailable to Apache; install alsa-utils and ensure it is on Apache\'s PATH.';
    } elseif (preg_match('/Invalid card number|Permission denied|cannot find card/i', $errorText)) {
        $message = 'Apache cannot open the ALSA card. Grant www-data access to /dev/snd (usually the audio group) and check Apache PrivateDevices.';
    } elseif ($configuredControl !== false && $configuredControl !== '') {
        $message = 'ALSA control "' . $configuredControl . '" was not found; check PI_AUDIO_CONTROL.';
    } else {
        $message = 'No ALSA playback mixer was found; check the VM sound device or ALSA permissions.';
    }
    $detail = trim(preg_replace('/\s+/', ' ', $errorText));
    if ($detail !== '') $message .= ' Probe result: ' . substr($detail, 0, 240);
    return ['target' => null, 'message' => $message];
}

function mixerCommand(array $arguments, ?array $target = null): array {
    if ($target === null) return ['ok' => false, 'output' => '', 'error' => 'No ALSA playback volume control found'];
    $command = ['amixer'];
    if ($target['card'] !== null) array_push($command, '-c', $target['card']);
    return runCommand(array_merge($command, $arguments));
}

function ensureDirectory(string $path): void {
    if (!is_dir($path) && !@mkdir($path, 0770, true) && !is_dir($path)) {
        throw new RuntimeException('Cannot create ' . $path . '. Check Apache directory permissions.');
    }
    if (!is_writable($path)) throw new RuntimeException('Apache cannot write to ' . $path . '.');
}

function readState(): array {
    $path = currentPath();
    if (!is_file($path)) return DEFAULT_STATE;
    $data = json_decode((string)file_get_contents($path), true);
    return is_array($data) ? normalizeState($data, DEFAULT_STATE) : DEFAULT_STATE;
}

function currentJsonIsValid(): bool {
    $contents = @file_get_contents(currentPath());
    return $contents !== false && is_array(json_decode($contents, true));
}

function writeJson(string $path, array $data): void {
    $directory = dirname($path);
    ensureDirectory($directory);
    $temporary = tempnam($directory, '.pi-audio-');
    if ($temporary === false) throw new RuntimeException('Could not create a temporary config file.');
    $json = json_encode($data, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR) . "\n";
    if (file_put_contents($temporary, $json, LOCK_EX) !== strlen($json) || !@rename($temporary, $path)) {
        @unlink($temporary);
        throw new RuntimeException('Could not write ' . $path . '.');
    }
    @chmod($path, 0664);
    $written = @file_get_contents($path);
    if ($written === false || !hash_equals($json, $written)) {
        throw new RuntimeException('Could not verify that ' . $path . ' was written.');
    }
}

function writeCurrentJson(array $state): bool {
    writeJson(currentPath(), $state);
    return true;
}

function cachedMixerStatus(): array {
    $cached = $_SESSION['mixerStatus'] ?? null;
    if (is_array($cached) && isset($cached['checkedAt'], $cached['status'])
        && is_int($cached['checkedAt']) && is_array($cached['status'])
        && time() - $cached['checkedAt'] < 600) {
        return $cached['status'];
    }
    $status = mixerStatus();
    $_SESSION['mixerStatus'] = ['checkedAt' => time(), 'status' => $status];
    return $status;
}

function runCommand(array $command, string $input = '', int $timeoutSeconds = 10): array {
    $process = @proc_open($command, [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, null, ['bypass_shell' => true]);
    if (!is_resource($process)) return ['ok' => false, 'output' => 'Command unavailable'];
    fwrite($pipes[0], $input);
    fclose($pipes[0]);
    stream_set_blocking($pipes[1], false);
    stream_set_blocking($pipes[2], false);
    $stdout = '';
    $stderr = '';
    $deadline = microtime(true) + $timeoutSeconds;
    $code = -1;
    do {
        $stdout .= stream_get_contents($pipes[1]);
        $stderr .= stream_get_contents($pipes[2]);
        $status = proc_get_status($process);
        if (!$status['running']) {
            $code = $status['exitcode'];
            break;
        }
        if (microtime(true) >= $deadline) {
            proc_terminate($process);
            fclose($pipes[1]);
            fclose($pipes[2]);
            proc_close($process);
            return ['ok' => false, 'output' => $stdout, 'error' => 'Command timed out'];
        }
        usleep(20000);
    } while (true);
    $stdout .= stream_get_contents($pipes[1]);
    $stderr .= stream_get_contents($pipes[2]);
    fclose($pipes[1]);
    fclose($pipes[2]);
    $closeCode = proc_close($process);
    if ($code < 0) $code = $closeCode;
    return ['ok' => $code === 0, 'output' => trim((string)$stdout), 'error' => trim((string)$stderr)];
}

function applyEngineConfiguration(array $state): array {
    $binary = getenv('PI_AUDIO_APPLY_BIN');
    if (!$binary) return ['ok' => false, 'message' => 'Audio engine adapter is not configured. Set PI_AUDIO_APPLY_BIN.'];
    if (!is_executable($binary)) return ['ok' => false, 'message' => 'Audio engine adapter is not executable by Apache: ' . $binary];
    $result = runCommand([$binary], json_encode($state, JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR), 30);
    $adapterResponse = json_decode($result['output'], true);
    if (is_array($adapterResponse)
        && (($adapterResponse['ok'] ?? null) === false
            || (is_string($adapterResponse['error'] ?? null) && $adapterResponse['error'] !== ''))) {
        $detail = $adapterResponse['error'] ?? null;
        if (!is_string($detail) || $detail === '') {
            $detail = is_string($adapterResponse['message'] ?? null) && $adapterResponse['message'] !== ''
                ? $adapterResponse['message']
                : 'Adapter reported an apply error.';
        }
        return ['ok' => false, 'message' => 'Audio engine apply failed: ' . $detail];
    }
    if (!$result['ok']) {
        $detail = $result['error'] ?: $result['output'] ?: 'Adapter exited unsuccessfully.';
        return ['ok' => false, 'message' => 'Audio engine apply failed: ' . $detail];
    }
    return ['ok' => true, 'message' => ''];
}

function dspConnectionStatus(): never {
    $binary = getenv('PI_AUDIO_STATUS_BIN');
    if (!$binary) respond(['error' => 'DSP status adapter is not configured. Set PI_AUDIO_STATUS_BIN.'], 503);
    if (!is_executable($binary)) respond(['error' => 'DSP status adapter is not executable by Apache: ' . $binary], 503);
    $result = runCommand([$binary], '', 3);
    if (!$result['ok']) {
        $detail = $result['error'] ?: $result['output'] ?: 'Status adapter exited unsuccessfully.';
        respond(['error' => 'DSP status probe failed: ' . $detail], 502);
    }
    $status = json_decode($result['output'], true);
    if (!is_array($status) || !is_bool($status['connected'] ?? null)
        || !array_key_exists('sampleRate', $status) || !array_key_exists('BufferSize', $status)) {
        respond(['error' => 'DSP status adapter must return JSON with boolean connected, sampleRate, and BufferSize fields.'], 502);
    }
    $sampleRate = $status['sampleRate'] ?? null;
    if ($sampleRate !== null && (!is_int($sampleRate) || !in_array($sampleRate, [44100, 48000, 88200, 96000, 176400, 192000], true))) {
        respond(['error' => 'DSP status adapter returned an unsupported sampleRate.'], 502);
    }
    if ($status['connected'] && $sampleRate === null) {
        respond(['error' => 'DSP status adapter must report sampleRate when connected.'], 502);
    }
    if (!$status['connected'] && $sampleRate !== null) {
        respond(['error' => 'DSP status adapter must report a null sampleRate when disconnected.'], 502);
    }
    $bufferSize = $status['BufferSize'] ?? null;
    if ($bufferSize !== null && (!is_int($bufferSize) || !in_array($bufferSize, [64, 128, 256, 512, 1024, 2048, 4096], true))) {
        respond(['error' => 'DSP status adapter returned an unsupported BufferSize.'], 502);
    }
    if ($status['connected'] && $bufferSize === null) {
        respond(['error' => 'DSP status adapter must report BufferSize when connected.'], 502);
    }
    if (!$status['connected'] && $bufferSize !== null) {
        respond(['error' => 'DSP status adapter must report a null BufferSize when disconnected.'], 502);
    }
    $message = $status['message'] ?? '';
    if (!is_string($message)) respond(['error' => 'DSP status adapter message must be a string.'], 502);
    respond([
        'connected' => $status['connected'],
        'sampleRate' => $sampleRate,
        'BufferSize' => $bufferSize,
        'message' => substr($message, 0, 240),
    ]);
}

function listPcms(string $utility): array {
    $result = runCommand([$utility, '-L']);
    if (!$result['ok']) return ['default'];
    $devices = [];
    foreach (preg_split('/\r?\n/', $result['output']) as $line) {
        if ($line !== '' && $line[0] !== ' ' && preg_match('/^[A-Za-z0-9_.:,-=]+$/', $line)) $devices[] = $line;
    }
    return array_values(array_unique(array_merge(['default'], $devices)));
}

function normalizeState(array $source, ?array $previous = null): array {
    $base = $previous ?? DEFAULT_STATE;
    $out = $base;
    $out['input'] = isset($source['input']) && is_string($source['input']) && preg_match('/^[A-Za-z0-9_.:,-=]{1,80}$/', $source['input']) ? $source['input'] : $base['input'];
    $out['output'] = isset($source['output']) && is_string($source['output']) && preg_match('/^[A-Za-z0-9_.:,-=]{1,80}$/', $source['output']) ? $source['output'] : $base['output'];
    $out['sampleRate'] = in_array($source['sampleRate'] ?? null, [44100, 48000, 88200, 96000, 176400, 192000], true) ? $source['sampleRate'] : $base['sampleRate'];
    $out['BufferSize'] = in_array($source['BufferSize'] ?? null, [64, 128, 256, 512, 1024, 2048, 4096], true) ? $source['BufferSize'] : $base['BufferSize'];
    $out['volume'] = max(0, min(100, (int)($source['volume'] ?? $base['volume'])));
    $out['muted'] = (bool)($source['muted'] ?? $base['muted']);
    $legacyCross = is_array($source['crossover'] ?? null) ? $source['crossover'] : [];
    $sourceCrossovers = is_array($source['crossovers'] ?? null) ? $source['crossovers'] : [];
    $out['crossovers'] = [];
    foreach (DEFAULT_STATE['crossovers'] as $name => $defaults) {
        $legacyName = str_replace('_', ' ', $name);
        $cross = is_array($sourceCrossovers[$name] ?? null)
            ? $sourceCrossovers[$name]
            : (is_array($sourceCrossovers[$legacyName] ?? null) ? $sourceCrossovers[$legacyName] : $legacyCross);
        $fallback = $base['crossovers'][$name] ?? $base['crossovers'][$legacyName] ?? $defaults;
        $out['crossovers'][$name] = [
            'frequency' => max(30, min(500, (int)($cross['frequency'] ?? $fallback['frequency']))),
            'topology' => in_array($cross['topology'] ?? '', ['Linkwitz-Riley', 'Butterworth', 'Bessel'], true) ? $cross['topology'] : $fallback['topology'],
            'slope' => in_array((int)($cross['slope'] ?? 0), [12, 18, 24, 36, 48], true) ? (int)$cross['slope'] : $fallback['slope'],
        ];
    }
    unset($out['crossover']);
    $sourceBands = is_array($source['bands'] ?? null) ? $source['bands'] : [];
    $isLegacyBandList = !$sourceBands || array_keys($sourceBands) === range(0, count($sourceBands) - 1);
    $legacyBands = $isLegacyBandList ? $sourceBands : null;
    $out['bands'] = [];
    foreach (array_keys(DEFAULT_STATE['bands']) as $driverName) {
        $driverBands = $legacyBands ?? ($sourceBands[$driverName] ?? $sourceBands[str_replace('_', ' ', $driverName)] ?? []);
        $out['bands'][$driverName] = [];
        foreach (array_slice(is_array($driverBands) ? $driverBands : [], 0, 12) as $band) {
            if (!is_array($band)) continue;
            $out['bands'][$driverName][] = [
                'id' => preg_match('/^[A-Za-z0-9_-]{1,32}$/', (string)($band['id'] ?? '')) ? $band['id'] : bin2hex(random_bytes(5)),
                'enabled' => (bool)($band['enabled'] ?? true),
                'frequency' => max(20, min(20000, (int)($band['frequency'] ?? 1000))),
                'q' => max(0.1, min(18, round((float)($band['q'] ?? 1), 2))),
                'gain' => max(-18, min(18, round((float)($band['gain'] ?? 0), 1))),
            ];
        }
    }
    $driverDefaults = DEFAULT_STATE['drivers'];
    $drivers = is_array($source['drivers'] ?? null) ? $source['drivers'] : [];
    $driversByName = [];
    foreach ($drivers as $item) {
        if (is_array($item) && is_string($item['name'] ?? null)) $driversByName[$item['name']] = $item;
    }
    $out['drivers'] = [];
    foreach ($driverDefaults as $driver) {
        $spacedName = str_replace('_', ' ', $driver['name']);
        $legacyName = ['Woofer_L' => 'Woofer', 'Tweeter_L' => 'Tweeter'][$driver['name']] ?? '';
        $item = $driversByName[$driver['name']] ?? $driversByName[$spacedName] ?? $driversByName[$legacyName] ?? [];
        $existing = null;
        foreach ($base['drivers'] as $baseDriver) {
            if (in_array($baseDriver['name'] ?? null, [$driver['name'], $spacedName, $legacyName], true)) { $existing = $baseDriver; break; }
        }
        $out['drivers'][] = [
            'name' => $driver['name'],
            'delayMs' => max(0, min(20, round((float)($item['delayMs'] ?? $existing['delayMs'] ?? $driver['delayMs']), 3))),
            'trimDb' => max(-24, min(12, round((float)($item['trimDb'] ?? $existing['trimDb'] ?? $driver['trimDb']), 1))),
            'phaseInverted' => is_bool($item['phaseInverted'] ?? null) ? $item['phaseInverted'] : (bool)($existing['phaseInverted'] ?? $driver['phaseInverted']),
        ];
    }
    $limiter = is_array($source['limiter'] ?? null) ? $source['limiter'] : $base['limiter'];
    $out['limiter'] = ['enabled' => (bool)($limiter['enabled'] ?? true), 'threshold' => max(-24, min(0, round((float)($limiter['threshold'] ?? -1), 1))), 'release' => max(10, min(2000, (int)($limiter['release'] ?? 80)))];
    $out['measurement'] = $base['measurement'] ?? null;
    if (is_array($source['measurement'] ?? null)) $out['measurement'] = validMeasurement($source['measurement']);
    if (is_array($source['roomCorrection'] ?? null)) {
        $out['roomCorrection'] = $source['roomCorrection'];
    } elseif (($source['roomCorrection'] ?? null) === null && array_key_exists('roomCorrection', $source)) {
        unset($out['roomCorrection']);
    }
    return $out;
}

function validPresetName(mixed $name): bool {
    return is_string($name) && preg_match('/^[A-Za-z0-9][A-Za-z0-9 _-]{0,31}$/', $name) === 1;
}

function presetPath(string $name): string {
    return presetsDir() . '/' . $name . '.json';
}

function findPresetPath(string $name): ?string {
    foreach (glob(presetsDir() . '/*.json') ?: [] as $path) {
        if (basename($path, '.json') === $name) return $path;
    }
    return null;
}

function readPresetVersions(string $name): array {
    $path = findPresetPath($name);
    if ($path === null) throw new RuntimeException('Preset not found.');
    $saved = json_decode((string)file_get_contents($path), true);
    if (!is_array($saved)) throw new RuntimeException('Preset file is not valid JSON.');
    if (!array_key_exists('staging', $saved) && !array_key_exists('production', $saved)) {
        $legacy = normalizeState($saved, DEFAULT_STATE);
        if (!array_key_exists('roomCorrection', $saved)) unset($legacy['roomCorrection']);
        $versions = ['staging' => $legacy, 'production' => $legacy];
        writeJson($path, $versions);
        return $versions;
    }
    if (!is_array($saved['staging'] ?? null) || !is_array($saved['production'] ?? null)) {
        throw new RuntimeException('Preset must contain staging and production configurations.');
    }
    $versions = [];
    foreach (['staging', 'production'] as $version) {
        $versions[$version] = normalizeState($saved[$version], DEFAULT_STATE);
        if (!array_key_exists('roomCorrection', $saved[$version])) unset($versions[$version]['roomCorrection']);
    }
    return $versions;
}

function presetNames(): array {
    $names = [];
    foreach (glob(presetsDir() . '/*.json') ?: [] as $path) $names[] = basename($path, '.json');
    if (!in_array('Default', $names, true) && findPresetPath('Default') === null) $names[] = 'Default';
    sort($names, SORT_NATURAL | SORT_FLAG_CASE);
    return $names;
}

function savePresetVersions(string $name, array $versions): void {
    writeJson(presetPath($name), $versions);
}

function savePresetStaging(string $name, array $state): void {
    $versions = readPresetVersions($name);
    $versions['staging'] = $state;
    savePresetVersions($name, $versions);
}

function stateSignature(array $state, string $version): string {
    if ($version === 'production') {
        $state = [
            'input' => $state['input'],
            'sampleRate' => $state['sampleRate'],
            'BufferSize' => $state['BufferSize'],
        ];
    }
    return hash('sha256', json_encode($state, JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR));
}

function refreshActiveSaved(string $name, string $version, array $snapshot): void {
    $active = $_SESSION['activeConfiguration'] ?? null;
    $signature = $_SESSION['activeConfigurationSignature'] ?? null;
    if (!is_array($active) || !is_string($signature)
        || ($active['name'] ?? null) !== $name || ($active['version'] ?? null) !== $version) return;
    $active['saved'] = hash_equals($signature, stateSignature($snapshot, $version));
    $_SESSION['activeConfiguration'] = $active;
}

function ensureDefaultPreset(): array {
    ensureDirectory(presetsDir());
    $path = presetPath('Default');
    if (findPresetPath('Default') === null) {
        $initial = is_file(currentPath()) ? readState() : normalizeState(DEFAULT_STATE, DEFAULT_STATE);
        savePresetVersions('Default', ['staging' => $initial, 'production' => $initial]);
    }
    $names = presetNames();
    $selected = $_SESSION['selectedPreset'] ?? 'Default';
    if (!in_array($selected, $names, true)) $selected = 'Default';
    $version = ($_SESSION['presetVersion'] ?? 'staging') === 'production' ? 'production' : 'staging';
    $_SESSION['selectedPreset'] = $selected;
    $_SESSION['presetVersion'] = $version;
    return ['names' => $names, 'selected' => $selected, 'version' => $version];
}

function validMeasurement(array $measurement): ?array {
    $responses = is_array($measurement['responses'] ?? null) ? $measurement['responses'] : [];
    if (!isset($responses['left']) && !isset($responses['right']) && is_array($measurement['response'] ?? null)) {
        $responses = ['left' => $measurement['response'], 'right' => $measurement['response']];
    }
    $validated = [];
    foreach (['left', 'right'] as $channel) {
        if (!is_array($responses[$channel] ?? null)) return null;
        $points = [];
        foreach (array_slice($responses[$channel], 0, 1000) as $point) {
            if (!is_array($point) || !is_numeric($point['hz'] ?? null) || !is_numeric($point['db'] ?? null)) continue;
            $hz = (float)$point['hz']; $db = (float)$point['db'];
            if ($hz >= 10 && $hz <= 40000 && $db >= -60 && $db <= 20) $points[] = ['hz' => $hz, 'db' => $db];
        }
        usort($points, fn($left, $right) => $left['hz'] <=> $right['hz']);
        if (count($points) < 2) return null;
        $validated[$channel] = $points;
    }
    return ['responses' => $validated, 'measuredAt' => (string)($measurement['measuredAt'] ?? date(DATE_ATOM))];
}

function metrics(): array {
    $cpuSample = static function (): ?array {
        $stat = @file_get_contents('/proc/stat');
        if ($stat === false) return null;
        if (!preg_match('/^cpu\s+(\d+(?:\s+\d+)*)/m', $stat, $match)) return null;
        $counters = array_map('intval', preg_split('/\s+/', trim($match[1])));
        if (!is_array($counters) || count($counters) < 4) return null;
        return ['total' => array_sum(array_slice($counters, 0, 8)), 'idle' => ($counters[3] ?? 0) + ($counters[4] ?? 0), 'time' => microtime(true)];
    };
    $cacheFile = sys_get_temp_dir() . '/pi-audio-cpu-sample';
    $prev = null;
    if (file_exists($cacheFile)) {
        $raw = @json_decode((string)@file_get_contents($cacheFile), true);
        if (is_array($raw) && isset($raw['total'], $raw['idle'], $raw['time']) && (microtime(true) - $raw['time']) < 30) {
            $prev = ['total' => $raw['total'], 'idle' => $raw['idle']];
        }
    }
    $current = $cpuSample();
    if ($current) {
        $tmp = tempnam(dirname($cacheFile), '.pi-audio-cpu-');
        if ($tmp !== false) {
            @file_put_contents($tmp, json_encode(['total' => $current['total'], 'idle' => $current['idle'], 'time' => $current['time']]));
            @rename($tmp, $cacheFile);
        }
    }
    $cpu = null;
    $cpuSource = null;
    if ($current !== null && $prev !== null) {
        $totalDelta = $current['total'] - $prev['total'];
        $idleDelta = $current['idle'] - $prev['idle'];
        if ($totalDelta > 0) {
            $cpu = max(0, min(100, (int)round(100 * ($totalDelta - $idleDelta) / $totalDelta)));
            $cpuSource = 'proc-stat';
        }
    }
    if ($cpu === null && function_exists('sys_getloadavg')) {
        $load = sys_getloadavg();
        if (is_array($load) && isset($load[0]) && is_numeric($load[0])) {
            $cpuInfo = @file_get_contents('/proc/cpuinfo');
            $cores = $cpuInfo !== false && preg_match_all('/^processor\s*:/m', $cpuInfo, $matches) ? count($matches[0]) : 1;
            $cpu = max(0, min(100, (int)round(100 * (float)$load[0] / max(1, $cores))));
            $cpuSource = 'loadavg';
        }
    }
    $rawTemp = @file_get_contents('/sys/class/thermal/thermal_zone0/temp');
    $temperature = is_numeric(trim((string)$rawTemp)) ? round((float)$rawTemp / 1000, 1) : null;
    $xrunPath = getenv('PI_AUDIO_XRUN_FILE') ?: '/run/pi-audio/xruns';
    $rawXruns = @file_get_contents($xrunPath);
    return ['cpu' => $cpu, 'cpuSource' => $cpuSource, 'cpuMessage' => $cpu === null ? 'CPU counters and load average are unavailable to PHP.' : '', 'temperature' => $temperature, 'xruns' => is_numeric(trim((string)$rawXruns)) ? max(0, (int)$rawXruns) : null];
}

function respond(array $data, int $status = 200): never {
    http_response_code($status);
    echo json_encode($data, JSON_UNESCAPED_SLASHES | JSON_INVALID_UTF8_SUBSTITUTE);
    exit;
}

try {
    $action = (string)($_GET['action'] ?? '');
    if ($action === 'state' && $_SERVER['REQUEST_METHOD'] === 'GET') {
        $presets = ensureDefaultPreset();
        $versions = readPresetVersions($presets['selected']);
        $state = is_file(currentPath()) ? readState() : $versions[$presets['version']];
        if (!is_file(currentPath())) writeCurrentJson($state);
        if (!isset($_SESSION['currentSource'])) {
            $snapshot = $versions[$presets['version']];
            $matches = $presets['version'] === 'production'
                ? $state['input'] === $snapshot['input']
                    && $state['sampleRate'] === $snapshot['sampleRate']
                    && $state['BufferSize'] === $snapshot['BufferSize']
                : $state === $snapshot;
            $_SESSION['currentSource'] = ['name' => $presets['selected'], 'version' => $presets['version'], 'saved' => $matches];
        }
        $alsa = cachedMixerStatus();
        $alsaTarget = $alsa['target'];
        respond(['state' => $state, 'currentJsonWritten' => currentJsonIsValid(), 'inputs' => listPcms('arecord'), 'outputs' => listPcms('aplay'), 'presets' => $presets['names'], 'selectedPreset' => $presets['selected'], 'presetVersion' => $presets['version'], 'currentSource' => $_SESSION['currentSource'] ?? ['name' => $presets['selected'], 'version' => $presets['version'], 'saved' => true], 'lastLoad' => $_SESSION['lastLoad'] ?? null, 'activeConfiguration' => $_SESSION['activeConfiguration'] ?? null, 'lastApplyAttempt' => $_SESSION['lastApplyAttempt'] ?? null, 'csrf' => $_SESSION['csrf'], 'measurementReady' => (bool)getenv('PI_AUDIO_MEASURE_BIN'), 'applyReady' => (bool)getenv('PI_AUDIO_APPLY_BIN') && is_executable((string)getenv('PI_AUDIO_APPLY_BIN')), 'alsaReady' => $alsaTarget !== null, 'alsaControl' => $alsaTarget === null ? null : $alsaTarget['control'] . ' (' . ($alsaTarget['card'] === null ? 'default device' : 'card ' . $alsaTarget['card']) . ')', 'alsaMessage' => $alsa['message'], 'serverTime' => time()]);
    }
    if ($action === 'metrics' && $_SERVER['REQUEST_METHOD'] === 'GET') respond(metrics());
    if ($action === 'dsp-status' && $_SERVER['REQUEST_METHOD'] === 'GET') dspConnectionStatus();
    if ($_SERVER['REQUEST_METHOD'] !== 'POST') respond(['error' => 'Method not allowed'], 405);
    if (!hash_equals((string)($_SESSION['csrf'] ?? ''), (string)($_SERVER['HTTP_X_CSRF_TOKEN'] ?? ''))) respond(['error' => 'Session expired. Reload the page.'], 403);
    $body = json_decode((string)file_get_contents('php://input'), true);
    if (!is_array($body)) respond(['error' => 'Expected a JSON request body.'], 400);

    if ($action === 'save') {
        $name = $body['presetName'] ?? $_SESSION['selectedPreset'] ?? 'Default';
        $version = $body['presetVersion'] ?? $_SESSION['presetVersion'] ?? 'staging';
        if (!validPresetName($name) || findPresetPath($name) === null) respond(['error' => 'Select a valid preset before saving.'], 422);
        if (!in_array($version, ['staging', 'production'], true)) respond(['error' => 'Invalid preset version.'], 422);
        $snapshotSaved = false;
        if ($version === 'production') {
            $versions = readPresetVersions($name);
            $state = normalizeState([
                'input' => $body['input'] ?? null,
                'sampleRate' => $body['sampleRate'] ?? null,
                'BufferSize' => $body['BufferSize'] ?? null,
            ], $versions['production']);
            $versions['production'] = $state;
            savePresetVersions($name, $versions);
            refreshActiveSaved($name, 'production', $state);
            $snapshotSaved = true;
        } else $state = normalizeState($body, readState());
        $_SESSION['selectedPreset'] = $name;
        $_SESSION['presetVersion'] = $version;
        $_SESSION['currentSource'] = ['name' => $name, 'version' => $version, 'saved' => $snapshotSaved];
        $currentJsonWritten = writeCurrentJson($state);
        respond(['state' => $state, 'currentSource' => $_SESSION['currentSource'], 'activeConfiguration' => $_SESSION['activeConfiguration'] ?? null, 'currentJsonWritten' => $currentJsonWritten]);
    }
    if ($action === 'save-staging') {
        $name = $body['presetName'] ?? $_SESSION['selectedPreset'] ?? 'Default';
        $version = $body['presetVersion'] ?? $_SESSION['presetVersion'] ?? 'staging';
        if (!validPresetName($name) || findPresetPath($name) === null) respond(['error' => 'Select a valid preset before saving staging.'], 422);
        if ($version !== 'staging') respond(['error' => 'Switch to staging before saving the staging snapshot.'], 409);
        $state = normalizeState($body, readState());
        $currentJsonWritten = writeCurrentJson($state);
        savePresetStaging($name, $state);
        refreshActiveSaved($name, 'staging', $state);
        $_SESSION['selectedPreset'] = $name;
        $_SESSION['presetVersion'] = 'staging';
        $_SESSION['currentSource'] = ['name' => $name, 'version' => 'staging', 'saved' => true];
        respond(['state' => $state, 'currentSource' => $_SESSION['currentSource'], 'activeConfiguration' => $_SESSION['activeConfiguration'] ?? null, 'currentJsonWritten' => $currentJsonWritten]);
    }
    if ($action === 'save-production') {
        $name = $body['presetName'] ?? $_SESSION['selectedPreset'] ?? 'Default';
        $version = $body['presetVersion'] ?? $_SESSION['presetVersion'] ?? 'production';
        if (!validPresetName($name) || findPresetPath($name) === null) respond(['error' => 'Select a valid preset before saving production.'], 422);
        if ($version !== 'production') respond(['error' => 'Switch to production before saving production settings.'], 409);
        $versions = readPresetVersions($name);
        $production = normalizeState([
            'input' => $body['input'] ?? null,
            'sampleRate' => $body['sampleRate'] ?? null,
            'BufferSize' => $body['BufferSize'] ?? null,
        ], $versions['production']);
        $versions['production'] = $production;
        savePresetVersions($name, $versions);
        refreshActiveSaved($name, 'production', $production);
        $state = normalizeState($body, readState());
        foreach (['input', 'sampleRate', 'BufferSize'] as $field) $state[$field] = $production[$field];
        $currentJsonWritten = writeCurrentJson($state);
        $_SESSION['selectedPreset'] = $name;
        $_SESSION['presetVersion'] = 'production';
        $_SESSION['currentSource'] = ['name' => $name, 'version' => 'production', 'saved' => true];
        respond(['state' => $state, 'currentSource' => $_SESSION['currentSource'], 'activeConfiguration' => $_SESSION['activeConfiguration'] ?? null, 'currentJsonWritten' => $currentJsonWritten]);
    }
    if ($action === 'master') {
        $name = $body['presetName'] ?? $_SESSION['selectedPreset'] ?? 'Default';
        $version = $body['presetVersion'] ?? $_SESSION['presetVersion'] ?? 'staging';
        if (!validPresetName($name) || !in_array($version, ['staging', 'production'], true)) respond(['error' => 'Select a valid preset before editing volume.'], 422);
        $state = normalizeState($body, readState());
        $_SESSION['selectedPreset'] = $name;
        $_SESSION['presetVersion'] = $version;
        $_SESSION['currentSource'] = ['name' => $name, 'version' => $version, 'saved' => false];
        $currentJsonWritten = writeCurrentJson($state);
        $alsa = cachedMixerStatus();
        $target = $alsa['target'];
        $result = $target === null ? ['ok' => false] : mixerCommand(['sset', $target['control'], $state['muted'] ? 'mute' : $state['volume'] . '%'], $target);
        respond(['state' => $state, 'currentSource' => $_SESSION['currentSource'], 'currentJsonWritten' => $currentJsonWritten, 'alsaApplied' => $result['ok'], 'alsaMessage' => $result['ok'] ? '' : 'Saved; no ALSA playback volume control was found.']);
    }
    if ($action === 'apply') {
        $name = $body['presetName'] ?? $_SESSION['selectedPreset'] ?? 'Default';
        $version = $body['presetVersion'] ?? $_SESSION['presetVersion'] ?? 'staging';
        if (!validPresetName($name) || findPresetPath($name) === null || !in_array($version, ['staging', 'production'], true)) respond(['error' => 'Select a valid preset version before applying.'], 422);
        $state = normalizeState($body, readState());
        $_SESSION['selectedPreset'] = $name;
        $_SESSION['presetVersion'] = $version;
        $versions = readPresetVersions($name);
        $saved = stateSignature($state, $version) === stateSignature($versions[$version], $version);
        $_SESSION['currentSource'] = ['name' => $name, 'version' => $version, 'saved' => $saved];
        $currentJsonWritten = writeCurrentJson($state);
        $_SESSION['lastLoad'] = ['snapshot' => $saved ? $name . ' ' . ucfirst($version) : 'unsaved current.json', 'at' => date(DATE_ATOM)];
        $_SESSION['lastApplyAttempt'] = ['name' => $name, 'version' => $version, 'success' => false, 'at' => date(DATE_ATOM)];
        $result = applyEngineConfiguration($state);
        if (!$result['ok']) respond(['error' => 'Configuration saved, but ' . $result['message'], 'currentJsonWritten' => $currentJsonWritten, 'lastLoad' => $_SESSION['lastLoad'], 'lastApplyAttempt' => $_SESSION['lastApplyAttempt']], 502);
        $_SESSION['lastApplyAttempt']['success'] = true;
        $_SESSION['activeConfiguration'] = ['name' => $name, 'version' => $version, 'saved' => $saved];
        $_SESSION['activeConfigurationSignature'] = stateSignature($state, $version);
        respond(['state' => $state, 'currentJsonWritten' => $currentJsonWritten, 'currentSource' => $_SESSION['currentSource'], 'activeConfiguration' => $_SESSION['activeConfiguration'], 'lastApplyAttempt' => $_SESSION['lastApplyAttempt'], 'lastLoad' => $_SESSION['lastLoad'], 'applied' => true]);
    }
    if ($action === 'test-speaker') {
        $driverName = $body['driver'] ?? '';
        $validNames = array_column(DEFAULT_STATE['drivers'], 'name');
        if (!is_string($driverName) || !in_array($driverName, $validNames, true)) respond(['error' => 'Invalid speaker test target.'], 422);
        $environmentVariable = 'PI_AUDIO_TEST_BIN_' . strtoupper($driverName);
        $binary = getenv($environmentVariable);
        if (!$binary) respond(['error' => 'Speaker test adapter is not configured. Set ' . $environmentVariable . '.'], 503);
        if (!is_executable($binary)) respond(['error' => 'Speaker test adapter is not executable by Apache: ' . $binary], 503);
        $state = readState();
        $payload = ['driver' => $driverName, 'output' => $state['output']];
        $result = runCommand([$binary], json_encode($payload, JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR), 15);
        if (!$result['ok']) {
            $detail = $result['error'] ?: $result['output'] ?: 'Adapter exited unsuccessfully.';
            respond(['error' => 'Speaker test failed: ' . $detail], 502);
        }
        respond(['tested' => $driverName]);
    }
    if ($action === 'preset-save') {
        $name = trim((string)($body['name'] ?? ''));
        if (!validPresetName($name)) respond(['error' => 'Use 1–32 letters, numbers, spaces, hyphens or underscores.'], 422);
        if ($name === 'Default') respond(['error' => 'The protected Default preset cannot be overwritten.'], 409);
        if (findPresetPath($name) !== null) respond(['error' => 'A preset with that name already exists. Select its staging version to edit it.'], 409);
        $state = normalizeState(is_array($body['state'] ?? null) ? $body['state'] : [], readState());
        savePresetVersions($name, ['staging' => $state, 'production' => $state]);
        $_SESSION['selectedPreset'] = $name;
        $_SESSION['presetVersion'] = 'staging';
        $currentJsonWritten = writeCurrentJson($state);
        $_SESSION['currentSource'] = ['name' => $name, 'version' => 'staging', 'saved' => true];
        respond(['presets' => presetNames(), 'selectedPreset' => $name, 'presetVersion' => 'staging', 'currentSource' => $_SESSION['currentSource'], 'currentJsonWritten' => $currentJsonWritten]);
    }
    if ($action === 'preset-select') {
        $name = (string)($body['name'] ?? '');
        $version = $body['version'] ?? 'staging';
        if (!validPresetName($name) || findPresetPath($name) === null) respond(['error' => 'Preset not found.'], 404);
        if (!in_array($version, ['staging', 'production'], true)) respond(['error' => 'Invalid preset version.'], 422);
        $versions = readPresetVersions($name);
        $_SESSION['selectedPreset'] = $name;
        $_SESSION['presetVersion'] = $version;
        $state = $versions[$version];
        $_SESSION['lastLoad'] = ['snapshot' => $name . ' ' . ucfirst($version), 'at' => date(DATE_ATOM)];
        $currentJsonWritten = writeCurrentJson($state);
        $_SESSION['currentSource'] = ['name' => $name, 'version' => $version, 'saved' => true];
        respond(['state' => $state, 'selectedPreset' => $name, 'presetVersion' => $version, 'lastLoad' => $_SESSION['lastLoad'], 'currentSource' => $_SESSION['currentSource'], 'currentJsonWritten' => $currentJsonWritten]);
    }
    if ($action === 'preset-promote') {
        $name = (string)($body['name'] ?? '');
        if (!validPresetName($name) || findPresetPath($name) === null) respond(['error' => 'Preset not found.'], 404);
        if (($body['version'] ?? '') !== 'staging') respond(['error' => 'Only staging can be promoted to production.'], 409);
        $versions = readPresetVersions($name);
        $versions['production'] = $versions['staging'];
        savePresetVersions($name, $versions);
        refreshActiveSaved($name, 'production', $versions['production']);
        $_SESSION['selectedPreset'] = $name;
        $_SESSION['presetVersion'] = 'production';
        $currentJsonWritten = writeCurrentJson($versions['production']);
        $_SESSION['currentSource'] = ['name' => $name, 'version' => 'production', 'saved' => true];
        $_SESSION['lastLoad'] = ['snapshot' => $name . ' Production', 'at' => date(DATE_ATOM)];
        respond(['state' => $versions['production'], 'selectedPreset' => $name, 'presetVersion' => 'production', 'currentSource' => $_SESSION['currentSource'], 'activeConfiguration' => $_SESSION['activeConfiguration'] ?? null, 'lastLoad' => $_SESSION['lastLoad'], 'currentJsonWritten' => $currentJsonWritten]);
    }
    if ($action === 'preset-copy-production-to-staging') {
        $name = (string)($body['name'] ?? '');
        if (!validPresetName($name) || findPresetPath($name) === null) respond(['error' => 'Preset not found.'], 404);
        if (($body['version'] ?? '') !== 'production') respond(['error' => 'Select production before copying it to staging.'], 409);
        $versions = readPresetVersions($name);
        $versions['staging'] = $versions['production'];
        savePresetVersions($name, $versions);
        refreshActiveSaved($name, 'staging', $versions['staging']);
        $_SESSION['selectedPreset'] = $name;
        $_SESSION['presetVersion'] = 'staging';
        $currentJsonWritten = writeCurrentJson($versions['staging']);
        $_SESSION['currentSource'] = ['name' => $name, 'version' => 'staging', 'saved' => true];
        $_SESSION['lastLoad'] = ['snapshot' => $name . ' Staging', 'at' => date(DATE_ATOM)];
        respond(['state' => $versions['staging'], 'selectedPreset' => $name, 'presetVersion' => 'staging', 'currentSource' => $_SESSION['currentSource'], 'activeConfiguration' => $_SESSION['activeConfiguration'] ?? null, 'lastLoad' => $_SESSION['lastLoad'], 'currentJsonWritten' => $currentJsonWritten]);
    }
    if ($action === 'preset-delete') {
        $name = (string)($body['name'] ?? '');
        if (!validPresetName($name)) respond(['error' => 'Invalid preset name.'], 422);
        $path = findPresetPath($name);
        if ($path === null) respond(['error' => 'Preset not found.'], 404);
        if (basename($path) === 'Default.json') respond(['error' => 'The protected Default preset cannot be deleted.'], 409);
        if (!@unlink($path)) respond(['error' => 'Could not remove preset.'], 500);
        if (($_SESSION['activeConfiguration']['name'] ?? null) === $name) {
            $_SESSION['activeConfiguration']['saved'] = false;
        }
        $selectedState = null;
        $currentJsonWritten = null;
        if (($_SESSION['selectedPreset'] ?? '') === $name) {
            $_SESSION['selectedPreset'] = 'Default';
            $_SESSION['presetVersion'] = 'staging';
            $selectedState = readPresetVersions('Default')['staging'];
            $currentJsonWritten = writeCurrentJson($selectedState);
            $_SESSION['currentSource'] = ['name' => 'Default', 'version' => 'staging', 'saved' => true];
            $_SESSION['lastLoad'] = ['snapshot' => 'Default Staging', 'at' => date(DATE_ATOM)];
        }
        $presets = presetNames();
        $response = ['presets' => $presets, 'selectedPreset' => $_SESSION['selectedPreset'] ?? 'Default', 'presetVersion' => $_SESSION['presetVersion'] ?? 'staging', 'currentSource' => $_SESSION['currentSource'] ?? null, 'activeConfiguration' => $_SESSION['activeConfiguration'] ?? null, 'lastLoad' => $_SESSION['lastLoad'] ?? null, 'state' => $selectedState];
        if ($currentJsonWritten !== null) $response['currentJsonWritten'] = $currentJsonWritten;
        respond($response);
    }
    if ($action === 'preset-delete-all') {
        ensureDirectory(presetsDir());
        $presetFiles = @glob(presetsDir() . '/*.json');
        if ($presetFiles === false) respond(['error' => 'Could not list saved presets.'], 500);
        $deleteErrors = [];
        foreach ($presetFiles as $path) {
            if (basename($path) === 'Default.json') continue;
            if (is_file($path) && !@unlink($path)) $deleteErrors[] = basename($path);
        }
        $presets = ensureDefaultPreset();
        $_SESSION['selectedPreset'] = 'Default';
        $_SESSION['presetVersion'] = 'staging';
        $state = readPresetVersions('Default')['staging'];
        if (isset($_SESSION['activeConfiguration']) && $_SESSION['activeConfiguration']['name'] !== 'Default') {
            $_SESSION['activeConfiguration']['saved'] = false;
        }
        refreshActiveSaved('Default', 'staging', $state);
        $currentJsonWritten = writeCurrentJson($state);
        $_SESSION['currentSource'] = ['name' => 'Default', 'version' => 'staging', 'saved' => true];
        $_SESSION['lastLoad'] = ['snapshot' => 'Default Staging', 'at' => date(DATE_ATOM)];
        $remainingFiles = @glob(presetsDir() . '/*.json');
        if ($remainingFiles === false) respond(['error' => 'Presets were processed, but the remaining presets could not be listed.'], 500);
        $presets = array_map(fn($path) => basename($path, '.json'), $remainingFiles);
        respond(['presets' => $presets, 'selectedPreset' => 'Default', 'presetVersion' => 'staging', 'currentSource' => $_SESSION['currentSource'], 'activeConfiguration' => $_SESSION['activeConfiguration'] ?? null, 'lastLoad' => $_SESSION['lastLoad'], 'deleted' => count($deleteErrors) === 0, 'deleteErrors' => $deleteErrors, 'state' => $state, 'currentJsonWritten' => $currentJsonWritten]);
    }
    if ($action === 'restore-defaults') {
        ensureDefaultPreset();
        $state = normalizeState(DEFAULT_STATE, DEFAULT_STATE);
        unset($state['roomCorrection']);
        $versions = readPresetVersions('Default');
        $versions['staging'] = $state;
        savePresetVersions('Default', $versions);
        refreshActiveSaved('Default', 'staging', $state);
        $_SESSION['selectedPreset'] = 'Default';
        $_SESSION['presetVersion'] = 'staging';
        $currentJsonWritten = writeCurrentJson($state);
        $_SESSION['currentSource'] = ['name' => 'Default', 'version' => 'staging', 'saved' => true];
        $_SESSION['lastLoad'] = ['snapshot' => 'Default Staging', 'at' => date(DATE_ATOM)];
        $mixer = cachedMixerStatus();
        $target = $mixer['target'];
        $alsa = $target === null
            ? ['ok' => false]
            : mixerCommand(['sset', $target['control'], $state['muted'] ? 'mute' : $state['volume'] . '%'], $target);
        respond(['state' => $state, 'currentSource' => $_SESSION['currentSource'], 'activeConfiguration' => $_SESSION['activeConfiguration'] ?? null, 'lastApplyAttempt' => $_SESSION['lastApplyAttempt'] ?? null, 'lastLoad' => $_SESSION['lastLoad'], 'currentJsonWritten' => $currentJsonWritten, 'alsaApplied' => $alsa['ok']]);
    }
    if ($action === 'measure') {
        $binary = getenv('PI_AUDIO_MEASURE_BIN');
        if (!$binary || !is_executable($binary)) respond(['error' => 'Room measurement is not configured. Set PI_AUDIO_MEASURE_BIN to an executable DSP measurement adapter.'], 503);
        $state = readState();
        $result = runCommand([$binary], json_encode($state, JSON_UNESCAPED_SLASHES), 180);
        $measurement = json_decode($result['output'] ?? '', true);
        $valid = is_array($measurement) ? validMeasurement($measurement) : null;
        $configuration = is_array($measurement['autoconfiguration'] ?? null) ? $measurement['autoconfiguration'] : null;
        $updates = [];
        foreach (['crossovers', 'crossover', 'bands', 'drivers'] as $key) {
            if ($configuration !== null && array_key_exists($key, $configuration)) $updates[$key] = $configuration[$key];
        }
        if (!$result['ok'] || $valid === null || !$updates) respond(['error' => $result['error'] ?: 'Measurement adapter must return a response curve and DSP autoconfiguration.'], 502);
        $state = normalizeState(array_replace($state, $updates), $state);
        $state['measurement'] = $valid;
        $currentJsonWritten = writeCurrentJson($state);
        respond(['state' => $state, 'currentJsonWritten' => $currentJsonWritten]);
    }
    respond(['error' => 'Unknown action.'], 404);
} catch (Throwable $error) {
    respond(['error' => $error->getMessage()], 500);
}