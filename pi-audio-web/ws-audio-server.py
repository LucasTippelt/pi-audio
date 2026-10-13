#!/usr/bin/env python3
"""Minimal WebSocket audio streaming server for pi-audio monitor."""

import asyncio
import struct
import hashlib
import os
import sys
import time
import zlib

HOST = "0.0.0.0"
PORT = 8765
HEADER_SIZE = 64
DATA_SIZE = 4 * 1024 * 1024
POLL_INTERVAL = 0.01
FRAME_SIZE = 4800

CHANNELS = {
    "pgm-l": "/run/pi-audio/monitor-pgm-l.raw",
    "pgm-r": "/run/pi-audio/monitor-pgm-r.raw",
    "in-l": "/run/pi-audio/monitor-in-l.raw",
    "in-r": "/run/pi-audio/monitor-in-r.raw",
    "out-l": "/run/pi-audio/monitor-out-l.raw",
    "out-r": "/run/pi-audio/monitor-out-r.raw",
    "woofer-l": "/run/pi-audio/monitor-woofer-l.raw",
    "woofer-r": "/run/pi-audio/monitor-woofer-r.raw",
    "tweeter-l": "/run/pi-audio/monitor-tweeter-l.raw",
    "tweeter-r": "/run/pi-audio/monitor-tweeter-r.raw",
    "mic": "/run/pi-audio/monitor-mic.raw",
}


def ws_frame(payload: bytes, opcode=0x2, compressed_data=None) -> bytes:
    if compressed_data is not None:
        data = b"\x80" + compressed_data
        rsv = 0x40
    else:
        data = payload
        rsv = 0x00
    length = len(data)
    if length < 126:
        return bytes([0x80 | rsv | opcode, length]) + data
    elif length < 65536:
        return bytes([0x80 | rsv | opcode, 126]) + struct.pack("!H", length) + data
    else:
        return bytes([0x80 | rsv | opcode, 127]) + struct.pack("!Q", length) + data


def ws_close_frame(code=1000, reason=b"") -> bytes:
    return ws_frame(struct.pack("!H", code) + reason, opcode=0x8)


def ws_pong_frame(payload: bytes) -> bytes:
    return ws_frame(payload, opcode=0xA)


def ws_handshake_response(key: bytes, origin: str = "", deflate_offer: str = "") -> bytes:
    magic = b"258EAFA5-E914-47DA-95CA-5AB5C0B3B184"
    accept = hashlib.sha1(key + magic).digest()
    accept_b64 = __import__("base64").b64encode(accept)
    resp = (
        b"HTTP/1.1 101 Switching Protocols\r\n"
        b"Upgrade: websocket\r\n"
        b"Connection: Upgrade\r\n"
        b"Sec-WebSocket-Accept: " + accept_b64 + b"\r\n"
    )
    # Do NOT echo Sec-WebSocket-Extensions — we send uncompressed frames
    # If we echo it, browser expects compressed frames and will drop connection
    resp += b"\r\n"
    return resp


class MonitorReader:
    """Keep file descriptor open, re-read header each poll."""

    def __init__(self, path: str):
        self.path = path
        self.fd = -1
        self.read_pos = 0
        self.open = False

    def open_file(self) -> bool:
        if self.open:
            return True
        try:
            self.fd = os.open(self.path, os.O_RDONLY)
            self.open = True
            return True
        except OSError:
            return False

    def close_file(self):
        if self.fd >= 0:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = -1
            self.open = False

    def read_data(self) -> bytes:
        if not self.open and not self.open_file():
            return b""
        if not self.open:
            return b""

        try:
            os.lseek(self.fd, 0, os.SEEK_SET)
            hdr = os.read(self.fd, HEADER_SIZE)
            if len(hdr) < HEADER_SIZE or hdr[0:4] != b"PIAM":
                return b""

            write_pos = struct.unpack_from("<Q", hdr, 6)[0]
            client_pos = self.read_pos

            if write_pos >= client_pos:
                avail = write_pos - client_pos
            else:
                avail = DATA_SIZE - client_pos + write_pos

            if avail < 4:
                return b""

            to_read = min(avail, FRAME_SIZE)
            to_read = (to_read // 4) * 4
            if to_read < 4:
                return b""

            file_offset = HEADER_SIZE + (client_pos % DATA_SIZE)
            seg_end = DATA_SIZE - (client_pos % DATA_SIZE)
            read_len = min(to_read, seg_end)
            read_len = (read_len // 4) * 4
            if read_len < 4:
                return b""

            os.lseek(self.fd, file_offset, os.SEEK_SET)
            chunk = os.read(self.fd, read_len)
            if not chunk:
                return b""

            total_read = len(chunk)
            client_pos = (client_pos + total_read) % DATA_SIZE

            remaining = to_read - total_read
            if remaining >= 4:
                wrap_wp = write_pos % DATA_SIZE
                new_cp = client_pos % DATA_SIZE
                if new_cp < wrap_wp:
                    wrap_read = min(remaining, wrap_wp - new_cp)
                    wrap_read = (wrap_read // 4) * 4
                    if wrap_read >= 4:
                        os.lseek(self.fd, HEADER_SIZE, os.SEEK_SET)
                        chunk2 = os.read(self.fd, wrap_read)
                        if chunk2:
                            chunk += chunk2
                            client_pos = (client_pos + len(chunk2)) % DATA_SIZE

            self.read_pos = client_pos
            return chunk
        except OSError:
            self.open = False
            self.fd = -1
            return b""


class WSReader:
    """Simple WebSocket frame reader using read() instead of readexactly()."""

    def __init__(self, stream_reader: asyncio.StreamReader, use_deflate=False):
        self._reader = stream_reader
        self._buf = bytearray()
        self._use_deflate = use_deflate
        self._decompressor = None
        if use_deflate:
            self._decompressor = zlib.decompressobj(-15)

    async def _ensure(self, n: int, timeout: float) -> bool:
        """Ensure at least n bytes in buffer. Returns False on timeout."""
        while len(self._buf) < n:
            try:
                data = await asyncio.wait_for(
                    self._reader.read(max(4096, n - len(self._buf))),
                    timeout=timeout
                )
            except asyncio.TimeoutError:
                return False
            except Exception:
                return False
            if not data:
                return False
            self._buf.extend(data)
        return True

    def _pop(self, n: int) -> bytes:
        result = bytes(self._buf[:n])
        del self._buf[:n]
        return result

    async def read_frame(self, timeout: float = 0.1):
        """Read one WebSocket frame. Returns (opcode, payload) or None."""
        if not await self._ensure(2, timeout):
            return None

        byte0, byte1 = self._buf[0], self._buf[1]
        self._raw_byte0 = byte0
        opcode = byte0 & 0x0F
        rsv = (byte0 >> 4) & 0x7
        masked = (byte1 & 0x80) != 0
        length = byte1 & 0x7F

        if length == 126:
            if not await self._ensure(4, timeout):
                return None
            length = struct.unpack(">H", self._buf[2:4])[0]
        elif length == 127:
            if not await self._ensure(10, timeout):
                return None
            length = struct.unpack(">Q", self._buf[2:10])[0]

        header_size = 2 if length < 126 else (4 if length < 65536 else 10)
        if masked:
            header_size += 4

        if not await self._ensure(header_size + length, timeout):
            return None

        payload_start = header_size
        if masked:
            self._pop(header_size - 4)
            mask_key = self._pop(4)
            raw = self._pop(length)
            payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(raw))
        else:
            self._pop(header_size)
            payload = self._pop(length)

        # Decompress if RSV1 is set (permessage-deflate)
        if rsv & 0x4 and self._decompressor and payload:
            bfe = payload[0]
            if bfe & 0x80:  # compressed
                payload = self._decompressor.decompress(payload[1:])

        return (opcode, payload)


async def handle_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    monitor_path = "/run/pi-audio/monitor-pgm-l.raw"
    channel_label = "pgm-l"
    peer = writer.get_extra_info("peername")
    frames_sent = 0
    bytes_sent = 0

    if writer.transport:
        writer.transport.set_write_buffer_limits(low=4096, high=32768)
        # Disable Nagle's algorithm to prevent TCP coalescing
        sock = writer.transport.get_extra_info('socket')
        if sock:
            try:
                sock.setsockopt(1, 1, 1)  # TCP_NODELAY
            except OSError:
                pass  # May fail in containers/sandboxes; not critical

    try:
        raw = await asyncio.wait_for(reader.read(4096), timeout=10)
        if not raw:
            writer.close()
            return

        request_str = raw.decode("utf-8", errors="ignore")
        first_line = request_str.split("\r\n")[0]
        parts = first_line.split(" ")

        key = ""
        origin = ""
        if len(parts) >= 2:
            url = parts[1]
            if "?" in url:
                for param in url.split("?", 1)[1].split("&"):
                    if param.startswith("ch="):
                        channel_label = param[3:]
                        monitor_path = CHANNELS.get(channel_label, monitor_path)
                        break

        for line in request_str.split("\r\n"):
            low = line.lower()
            if low.startswith("sec-websocket-key:"):
                key = line.split(":", 1)[1].strip()
            elif low.startswith("origin:"):
                origin = line.split(":", 1)[1].strip()

        if not key:
            writer.write(b"HTTP/1.1 400 Bad Request\r\n\r\n")
            await writer.drain()
            writer.close()
            return

        # Check if client wants permessage-deflate — echo back their exact offer
        has_deflate = False
        deflate_offer = ""
        for line in request_str.split("\r\n"):
            if line.lower().startswith("sec-websocket-extensions:"):
                if "permessage-deflate" in line:
                    has_deflate = True
                    deflate_offer = line.split(":", 1)[1].strip()
                break

        print(f"[{peer}] +{channel_label} origin={origin} client-deflate={has_deflate} offer={deflate_offer}")

        resp = ws_handshake_response(key.encode(), origin, deflate_offer=deflate_offer)
        writer.write(resp)
        await writer.drain()
        print(f"[{peer}] handshake done, eof={reader.at_eof()}", flush=True)

        ws_reader = WSReader(reader, use_deflate=has_deflate)
        mon = MonitorReader(monitor_path)
        stop = asyncio.Event()
        last_activity = time.monotonic()
        compressor = None
        if has_deflate:
            compressor = zlib.compressobj(zlib.Z_DEFAULT_COMPRESSION, zlib.DEFLATED, -15)

        async def reader_task():
            nonlocal last_activity
            try:
                while not stop.is_set():
                    result = await ws_reader.read_frame(timeout=0.05)
                    if result is None:
                        await asyncio.sleep(0.05)
                        continue
                    last_activity = time.monotonic()
                    opcode, payload = result
                    raw_b0 = getattr(ws_reader, '_raw_byte0', 0)
                    print(f"[{peer}] rx byte0=0x{raw_b0:x} opcode=0x{opcode:x} len={len(payload)}", file=sys.stderr)
                    if opcode == 0x8:
                        print(f"[{peer}] client close", file=sys.stderr)
                        stop.set()
                        break
                    elif opcode == 0x9:
                        try:
                            writer.write(ws_pong_frame(payload))
                            await writer.drain()
                        except Exception:
                            stop.set()
            except asyncio.CancelledError:
                raise
            except Exception as e:
                print(f"[{peer}] reader_task err: {e}", file=sys.stderr)
                stop.set()

        rt = asyncio.create_task(reader_task())
        print(f"[{peer}] streaming start", flush=True)
        await asyncio.sleep(0.5)  # Wait for browser to complete WebSocket setup

        try:
            frame_count = 0
            while not stop.is_set():
                await asyncio.sleep(POLL_INTERVAL)
                # Use writer.is_closing() instead of reader.at_eof() for reliability
                if writer.is_closing():
                    print(f"[{peer}] writer closing in loop", file=sys.stderr)
                    break

                if writer.transport and writer.transport.is_closing():
                    print(f"[{peer}] transport closing in loop", file=sys.stderr)
                    break

                data = mon.read_data()
                if data:
                    try:
                        compressed = None
                        # TODO: re-enable compression once format is verified
                        # if compressor:
                        #     compressed = compressor.compress(data) + compressor.flush(zlib.Z_SYNC_FLUSH)
                        frame = ws_frame(data, compressed_data=compressed)
                        writer.write(frame)
                        await writer.drain()
                        frames_sent += 1
                        bytes_sent += len(data)
                        frame_count += 1
                        if frame_count <= 3:
                            print(f"[{peer}] sent frame #{frame_count} sz={len(data)}", file=sys.stderr)

                        buf_sz = writer.transport.get_write_buffer_size()
                        if buf_sz > 65536:
                            await asyncio.sleep(0.1)
                        elif buf_sz > 131072:
                            print(f"[{peer}] buf-full {buf_sz}")
                            break
                    except (ConnectionError, BrokenPipeError, OSError):
                        break
        finally:
            print(f"[{peer}] stream end: stop={stop.is_set()} writer_closing={writer.is_closing()} f={frames_sent} b={bytes_sent}", file=sys.stderr)
            stop.set()
            rt.cancel()
            try:
                await rt
            except Exception:
                pass
            mon.close_file()

    except asyncio.CancelledError:
        pass
    except asyncio.TimeoutError:
        pass
    except Exception as e:
        print(f"[{peer}] err: {e}", file=sys.stderr)
    finally:
        try:
            if not writer.is_closing():
                writer.write(ws_close_frame(1000, b""))
                await writer.drain()
        except Exception:
            pass
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass
        print(f"[{peer}] -{channel_label} f={frames_sent} b={bytes_sent}")


async def main():
    server = await asyncio.start_server(handle_client, HOST, PORT)
    print(f"WebSocket audio server on ws://{HOST}:{PORT}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nServer stopped.", flush=True)
