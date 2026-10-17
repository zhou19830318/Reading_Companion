#!/usr/bin/env python3
"""Minimal Voice Relay test client (docs section 17.13).

No ESP32 required. Sends the device protocol over WS/WSS and prints every
server event with arrival timestamps relative to audio.end.

  * WSS TLS verification on by default (--insecure only for local debugging)
  * Authorization: Bearer <token>, X-Device-Id, X-Voice-Protocol: 1
  * session.start -> audio.start -> N x 40 ms PCM frames -> audio.end
  * prints asr.partial / asr.final / llm.delta / session.done
  * reports a timeout when no event arrives within --event-timeout seconds
  * sends session.cancel (or clean close) before exiting

Audio source: --pcm <file> (raw s16le mono 16 kHz or a PCM WAV) or
--tone <seconds> to synthesize a speech-band tone. The mock ASR/LLM pair
accepts anything; the real provider needs real speech.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import ssl
import struct
import sys
import time
import uuid
import wave

try:
    import websockets
except ImportError:  # pragma: no cover
    print("pip install websockets", file=sys.stderr)
    raise

PROTOCOL_VERSION = "1"
FRAME_MS = 40
SAMPLE_RATE = 16000
FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS // 1000
FRAME_BYTES = FRAME_SAMPLES * 2


def make_tone(seconds: float) -> bytes:
    """Speech-band buzzy tone so VAD-ish upstreams get non-silence."""
    n = int(SAMPLE_RATE * seconds)
    out = bytearray()
    for i in range(n):
        t = i / SAMPLE_RATE
        # 220 Hz fundamental + 440 Hz harmonic, amplitude-modulated at 4 Hz
        v = 0.35 * (0.6 * __import__("math").sin(2 * 3.14159265 * 220 * t)
                    + 0.4 * __import__("math").sin(2 * 3.14159265 * 440 * t))
        v *= 0.6 + 0.4 * __import__("math").sin(2 * 3.14159265 * 4 * t)
        out += struct.pack("<h", int(v * 30000))
    return bytes(out)


def load_pcm(path: str) -> bytes:
    with open(path, "rb") as fh:
        head = fh.read(4)
    if head[:4] == b"RIFF":
        with wave.open(path, "rb") as wf:
            assert wf.getnchannels() == 1, "mono only"
            assert wf.getframerate() == SAMPLE_RATE, "16 kHz only"
            assert wf.getsampwidth() == 2, "s16le only"
            return wf.readframes(wf.getnframes())
    with open(path, "rb") as fh:
        return fh.read()


async def run(args: argparse.Namespace) -> int:
    pcm = load_pcm(args.pcm) if args.pcm else make_tone(args.tone)
    frames = [pcm[i : i + FRAME_BYTES] for i in range(0, len(pcm), FRAME_BYTES)]
    request_id = uuid.uuid4().hex
    session_id_holder = {"id": None}

    ssl_ctx = None
    if args.url.startswith("wss://"):
        ssl_ctx = ssl.create_default_context()
        if args.insecure:
            ssl_ctx.check_hostname = False
            ssl_ctx.verify_mode = ssl.CERT_NONE

    headers = {
        "Authorization": f"Bearer {args.token}",
        "X-Device-Id": args.device_id,
        "X-Voice-Protocol": PROTOCOL_VERSION,
    }

    t_audio_end: float | None = None
    last_event = time.monotonic()
    got_final = asyncio.Event()
    done = asyncio.Event()

    async with websockets.connect(
        args.url,
        additional_headers=headers,
        ssl=ssl_ctx or None,
        open_timeout=5,
        close_timeout=2,
        max_size=1 << 20,
    ) as ws:
        async def printer():
            nonlocal last_event
            while True:
                raw = await ws.recv()
                last_event = time.monotonic()
                if isinstance(raw, bytes):
                    print(f"<binary {len(raw)}B>")
                    continue
                try:
                    msg = json.loads(raw)
                except json.JSONDecodeError:
                    print(f"<bad json> {raw[:120]!r}")
                    continue
                mtype = msg.get("type")
                rel = f"+{(last_event - t_audio_end) * 1000:7.1f} ms" if t_audio_end else "        "
                if mtype == "asr.partial":
                    print(f"{rel} asr.partial  seq={msg.get('seq')} {msg.get('text')!r}")
                elif mtype == "asr.final":
                    print(f"{rel} asr.final    {msg.get('text')!r}")
                elif mtype == "llm.start":
                    print(f"{rel} llm.start")
                elif mtype == "llm.delta":
                    print(f"{rel} llm.delta    seq={msg.get('seq')} {msg.get('text')!r}")
                elif mtype == "llm.final":
                    print(f"{rel} llm.final    {msg.get('text')!r} usage={msg.get('usage')}")
                elif mtype == "session.done":
                    print(f"{rel} session.done latencyMs={json.dumps(msg.get('latencyMs'), ensure_ascii=False)}")
                    got_final.set()
                    done.set()
                elif mtype == "error":
                    print(f"{rel} ERROR        code={msg.get('code')} retryable={msg.get('retryable')} msg={msg.get('message')}")
                    done.set()
                else:
                    print(f"{rel} {mtype}: {json.dumps(msg, ensure_ascii=False)[:160]}")

        printer_task = asyncio.create_task(printer())

        async def watchdog():
            while not done.is_set():
                await asyncio.sleep(0.25)
                if time.monotonic() - last_event > args.event_timeout:
                    print(f"!! no server event for {args.event_timeout}s — TIMEOUT")
                    done.set()

        watchdog_task = asyncio.create_task(watchdog())

        await ws.send(json.dumps({
            "type": "session.start",
            "requestId": request_id,
            "deviceId": args.device_id,
            "audio": {"sampleRate": SAMPLE_RATE, "channels": 1, "format": "pcm_s16le", "frameMs": FRAME_MS},
            "mode": "relay",
        }))
        await asyncio.sleep(0.05)
        await ws.send(json.dumps({"type": "audio.start", "requestId": request_id, "sessionId": ""}))

        t_send = time.monotonic()
        for i, frame in enumerate(frames):
            await ws.send(frame)
            if args.realtime:
                await asyncio.sleep(FRAME_MS / 1000)
            if i == 4:
                print(f"audio streaming: {len(frames)} frames ({len(pcm)/32:.1f} ms), "
                      f"realtime={args.realtime}")
        t_audio_end = time.monotonic()
        await ws.send(json.dumps({
            "type": "audio.end", "requestId": request_id,
            "sessionId": session_id_holder["id"] or "", "reason": "vad_silence",
        }))
        print(f"audio.end sent after {(t_audio_end - t_send) * 1000:.0f} ms")

        try:
            await asyncio.wait_for(done.wait(), timeout=args.total_timeout)
        except asyncio.TimeoutError:
            print("!! session did not complete in time")
        if args.cancel and not got_final.is_set():
            await ws.send(json.dumps({
                "type": "session.cancel", "requestId": request_id,
                "sessionId": session_id_holder["id"] or "", "reason": "user_cancel",
            }))
            print("session.cancel sent")
        printer_task.cancel()
        watchdog_task.cancel()
        for t in (printer_task, watchdog_task):
            try:
                await t
            except asyncio.CancelledError:
                pass
    return 0 if got_final.is_set() else 1


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--url", default=os.environ.get("RELAY_URL", "ws://127.0.0.1:8000/api/v1/voice/stream"))
    p.add_argument("--token", default=os.environ.get("RELAY_DEVICE_TOKEN", ""))
    p.add_argument("--device-id", default=os.environ.get("RELAY_DEVICE_ID", "debug-client"))
    p.add_argument("--pcm", default="", help="raw s16le mono 16k PCM or PCM WAV file")
    p.add_argument("--tone", type=float, default=2.0, help="synthesize N seconds of tone")
    p.add_argument("--realtime", action="store_true", help="pace frames at 40 ms like the device")
    p.add_argument("--event-timeout", type=float, default=5.0)
    p.add_argument("--total-timeout", type=float, default=40.0)
    p.add_argument("--cancel", action="store_true", help="cancel before exit when no final")
    p.add_argument("--insecure", action="store_true", help="skip TLS verification (debug only)")
    args = p.parse_args()
    if not args.token:
        print("--token or RELAY_DEVICE_TOKEN required", file=sys.stderr)
        sys.exit(2)
    sys.exit(asyncio.run(run(args)))


if __name__ == "__main__":
    main()
