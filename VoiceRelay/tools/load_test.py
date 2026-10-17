#!/usr/bin/env python3
"""Multi-device load test for the Voice Relay (docs "multiple onepage" case).

Each client opens ONE WebSocket and runs --rounds sequential voice sessions on
it with the same real speech sample, exercising socket reuse and the relay's
capacity limits. Summary statistics are printed at the end; exit code is
non-zero if any round failed.

    RELAY_DEVICE_TOKEN=... python tools/load_test.py --pcm speech.pcm
    RELAY_DEVICE_TOKEN=... python tools/load_test.py --pcm speech.pcm --clients 4 --rounds 5
    RELAY_DEVICE_TOKEN=... python tools/load_test.py --pcm speech.pcm --realtime
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import time

import websockets

FRAME_BYTES = 1280  # 40 ms @ 16 kHz mono s16le
FRAME_MS = 40


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--url", default=os.environ.get("RELAY_URL", "ws://127.0.0.1:8080/api/v1/voice/stream"))
    p.add_argument("--token", default=os.environ.get("RELAY_DEVICE_TOKEN", ""))
    p.add_argument("--pcm", required=True, help="raw s16le mono 16 kHz PCM speech sample")
    p.add_argument("--clients", type=int, default=1, help="concurrent devices (each with one socket)")
    p.add_argument("--rounds", type=int, default=1, help="sequential sessions per client")
    p.add_argument("--realtime", action="store_true", help="pace frames at 40 ms like a real device")
    p.add_argument("--round-timeout", type=float, default=30.0)
    return p.parse_args()


async def run_round(ws, pcm: bytes, device: str, rnd: int, realtime: bool, timeout: float) -> dict:
    request_id = f"{device}-{rnd}"
    await ws.send(
        json.dumps(
            {
                "type": "session.start",
                "requestId": request_id,
                "deviceId": device,
                "audio": {"sampleRate": 16000, "channels": 1, "format": "pcm_s16le", "frameMs": FRAME_MS},
                "mode": "relay",
            }
        )
    )
    session_id = ""
    reply_chars = 0
    while not session_id:
        msg = json.loads(await asyncio.wait_for(ws.recv(), timeout))
        if msg.get("type") == "session.started":
            session_id = msg.get("sessionId") or ""
        elif msg.get("type") == "error":
            return {"ok": False, "error": msg.get("code")}

    await ws.send(json.dumps({"type": "audio.start", "requestId": request_id, "sessionId": session_id}))
    for off in range(0, len(pcm), FRAME_BYTES):
        await ws.send(pcm[off : off + FRAME_BYTES])
        if realtime:
            await asyncio.sleep(FRAME_MS / 1000)
    await ws.send(
        json.dumps({"type": "audio.end", "requestId": request_id, "sessionId": session_id, "reason": "vad_silence"})
    )

    while True:
        msg = json.loads(await asyncio.wait_for(ws.recv(), timeout))
        if msg.get("type") == "llm.delta":
            reply_chars += len(msg.get("text") or "")
        elif msg.get("type") == "session.done":
            return {"ok": True, "replyChars": reply_chars, **(msg.get("latencyMs") or {})}
        elif msg.get("type") == "error":
            return {"ok": False, "error": msg.get("code")}


async def client_task(args: argparse.Namespace, pcm: bytes, cid: int) -> list[dict]:
    device = f"load-{cid}"
    headers = {"Authorization": f"Bearer {args.token}", "X-Device-Id": device, "X-Voice-Protocol": "1"}
    results: list[dict] = []
    async with websockets.connect(args.url, additional_headers=headers, max_size=1 << 20) as ws:
        for rnd in range(1, args.rounds + 1):
            t0 = time.monotonic()
            try:
                res = await run_round(ws, pcm, device, rnd, args.realtime, args.round_timeout)
            except asyncio.TimeoutError:
                res = {"ok": False, "error": "TIMEOUT"}
            res["round"] = rnd
            res["wallMs"] = int((time.monotonic() - t0) * 1000)
            results.append(res)
    return results


async def main() -> int:
    args = parse_args()
    if not args.token:
        print("missing --token or RELAY_DEVICE_TOKEN", file=sys.stderr)
        return 2
    pcm = open(args.pcm, "rb").read()
    print(f"clients={args.clients} rounds={args.rounds} realtime={args.realtime} audio={len(pcm)//32}ms")

    all_results = await asyncio.gather(*(client_task(args, pcm, c) for c in range(1, args.clients + 1)))
    results = [r for per_client in all_results for r in per_client]
    failures = [r for r in results if not r["ok"]]
    for r in results:
        if not r["ok"]:
            print(f"round {r['round']}: FAIL error={r['error']}")

    print(f"\nsuccess: {len(results) - len(failures)}/{len(results)}")
    ok = [r for r in results if r["ok"]]
    for key in ("asrFirstPartial", "asrFinal", "llmFirstToken", "llmFinal", "visibleTotal", "wallMs"):
        vals = [r[key] for r in ok if key in r]
        if vals:
            print(f"{key:15s} min={min(vals)} avg={sum(vals)//len(vals)} max={max(vals)}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
