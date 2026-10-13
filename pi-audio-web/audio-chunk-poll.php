<?php
declare(strict_types=1);
opcache_invalidate(__FILE__, true);

/* Long-poll chunk endpoint.
 * Each request accumulates data for ~1 second, then returns it all.
 * Client immediately fetches the next chunk.
 * Much lower RTT overhead than one-fetch-per-chunk. */

$runDir = '/run/pi-audio';
$channel = $_GET['ch'] ?? 'pgm-l';
$maxBytes = intval($_GET['maxBytes'] ?? 384000); /* default 2s of 48kHz mono float32 */

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

/* Client-tracked read position. */
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

/* Wait for data to become available, then read it all.
 * Waits up to $waitTimeout seconds — avoids empty responses. */
$waitTimeout = floatval($_GET['wait'] ?? 0.05);
$output = '';
$totalRead = 0;
$deadline = microtime(true) + $waitTimeout;

do {
    /* Refresh writePos from header. */
    fseek($fp, 0);
    $hdr = fread($fp, $kHeaderSize);
    if (!$hdr || substr($hdr, 0, 4) !== 'PIAM') break;

    $wL = unpack('V', substr($hdr, 6, 4))[1];
    $wH = unpack('V', substr($hdr, 10, 4))[1];
    $writePos = ($wH << 32) | $wL;

    /* Calculate available data. */
    if ($writePos >= $readPos) {
        $avail = $writePos - $readPos;
    } else {
        $avail = $dataSize - $readPos + $writePos;
    }

    if ($avail >= 4) {
        $toRead = min($avail, $maxBytes);
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
                $output .= $chunk;
                $readPos = ($readPos + $len) % $dataSize;
                $totalRead += $len;

                /* Second segment if ring wrap. */
                $remaining = $toRead - $len;
                if ($remaining >= 4) {
                    $wrapWritePos = $writePos % $dataSize;
                    if ($readPos < $wrapWritePos) {
                        fseek($fp, $kHeaderSize);
                        $chunk2 = fread($fp, $remaining);
                        if ($chunk2 && strlen($chunk2) > 0) {
                            $len2 = strlen($chunk2);
                            $output .= $chunk2;
                            $readPos = ($readPos + $len2) % $dataSize;
                            $totalRead += $len2;
                        }
                    }
                }
                break; /* Got data — return immediately. */
            }
        }
    }

    /* No data yet — wait briefly and retry. */
    usleep(50000); /* 50ms */
} while (microtime(true) < $deadline);

fclose($fp);

$totalSent = strlen($output);
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
    echo $output;
}
