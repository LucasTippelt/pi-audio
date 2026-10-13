<?php
declare(strict_types=1);
opcache_invalidate(__FILE__, true);

$runDir = '/run/pi-audio';
$channel = $_GET['ch'] ?? 'pgm-l';
$bytes = intval($_GET['bytes'] ?? 96000);

$validChannels = ['pgm-l', 'pgm-r', 'woofer-l', 'tweeter-l', 'woofer-r', 'tweeter-r', 'mic'];
if (!in_array($channel, $validChannels)) {
    http_response_code(400);
    exit;
}

$monitorPath = $runDir . '/monitor-' . $channel . '.raw';
if (!is_file($monitorPath)) {
    http_response_code(503);
    exit;
}

$kHeaderSize = 64;
$dataSize = 4 * 1024 * 1024;

$fp = fopen($monitorPath, 'rb');
if (!$fp) {
    http_response_code(500);
    exit;
}

fseek($fp, 0);
$hdr = fread($fp, $kHeaderSize);

if (!$hdr || strlen($hdr) < 24 || substr($hdr, 0, 4) !== 'PIAM') {
    fclose($fp);
    http_response_code(503);
    exit;
}

$wL = unpack('V', substr($hdr, 6, 4))[1];
$wH = unpack('V', substr($hdr, 10, 4))[1];
$writePos = ($wH << 32) | $wL;
$sampleRate = unpack('V', substr($hdr, 14, 4))[1];
$seq = unpack('V', substr($hdr, 20, 4))[1];

/* Client-tracked read position for gapless playback. */
$readPos = $writePos;
if (isset($_GET['pos'])) {
    $pos = intval($_GET['pos']);
    $posH = intval($_GET['posH'] ?? 0);
    $clientPos = ($posH << 32) | ($pos & 0xFFFFFFFF);
    $halfRing = $dataSize / 2;
    if ($writePos >= $clientPos) {
        $gap = $writePos - $clientPos;
    } else {
        $gap = $dataSize - $clientPos + $writePos;
    }
    if ($gap > 0 && $gap < $halfRing) {
        $readPos = $clientPos;
    }
}

/* Read available data ONCE — no waiting. Return immediately. */
if ($writePos >= $readPos) {
    $avail = $writePos - $readPos;
} else {
    $avail = $dataSize - $readPos + $writePos;
}

$buffer = '';
if ($avail >= 4) {
    $toRead = min($avail, $bytes);
    $toRead = (int)floor($toRead / 4) * 4;

    $fileOffset = $kHeaderSize + ($readPos % $dataSize);
    $segEnd = $dataSize - ($readPos % $dataSize);
    $readLen = min($toRead, $segEnd);
    $readLen = (int)floor($readLen / 4) * 4;

    if ($readLen >= 4) {
        fseek($fp, $fileOffset);
        $chunk = fread($fp, $readLen);
        if ($chunk && strlen($chunk) > 0) {
            $len = strlen($chunk);
            $buffer .= $chunk;
            $readPos = ($readPos + $len) % $dataSize;
            $remaining = $toRead - $len;

            /* Second segment if we wrapped around the ring buffer. */
            if ($remaining >= 4 && $readPos < $writePos % $dataSize) {
                fseek($fp, $kHeaderSize);
                $chunk2 = fread($fp, $remaining);
                if ($chunk2 && strlen($chunk2) > 0) {
                    $len2 = strlen($chunk2);
                    $buffer .= $chunk2;
                    $readPos = ($readPos + $len2) % $dataSize;
                }
            }
        }
    }
}

$totalSent = strlen($buffer);
$posL = (int)($readPos & 0xFFFFFFFF);
$posH = (int)($readPos >> 32);

header('Content-Type: audio/x-raw');
header('Cache-Control: no-cache');
header('Content-Length: ' . $totalSent);
header('X-Sample-Rate: ' . $sampleRate);
header('X-Channels: 1');
header('X-Seq: ' . $seq);
header('X-Read-Pos: ' . $posL);
header('X-Read-PosH: ' . $posH);

if ($totalSent > 0) {
    echo $buffer;
}
fclose($fp);
