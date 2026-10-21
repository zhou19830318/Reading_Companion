"""Loopback-only setup page shared by the Linux and Windows deployments.

The page answers the one question a first-time installer has: what do I paste
into the reader's Settings -> Voice Relay card? It is reachable only from the
host itself (127.0.0.1 / ::1) so the device token never leaks to the LAN by
default. On a headless Ubuntu box use an SSH tunnel:

    ssh -L 8000:127.0.0.1:8000 user@server
    open http://127.0.0.1:8000/admin

When RELAY_ADMIN_TOKEN is set the page also accepts
`?token=<admin-token>` / `X-Admin-Token:` so an operator who knows the admin
secret can read it remotely. nginx configs deliberately do not proxy /admin.
"""

from __future__ import annotations

import ipaddress
import socket

from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse, JSONResponse

from . import SERVICE_NAME, __version__
from .config import Settings, get_settings

LOOPBACK_HOSTS = {"127.0.0.1", "::1", "::ffff:127.0.0.1"}
DEFAULT_DEVICE_ID = "reading-companion-001"


def _is_loopback(request: Request) -> bool:
    client = request.client
    return client is not None and client.host in LOOPBACK_HOSTS


def _authorized(request: Request, settings: Settings) -> bool:
    if _is_loopback(request):
        return True
    token = settings.relay_admin_token
    if not token:
        return False
    supplied = request.headers.get("x-admin-token") or request.query_params.get("token")
    return supplied == token


def _lan_ips() -> list[str]:
    """Best-effort LAN addresses. The UDP probe sends no packets; it only asks
    the routing table which local address the default route would use, which
    works on both Linux and Windows."""
    found: list[str] = []
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect(("8.8.8.8", 80))
            found.append(probe.getsockname()[0])
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            found.append(info[4][0])
    except OSError:
        pass
    unique: list[str] = []
    for ip in found:
        if ip in unique:
            continue
        try:
            addr = ipaddress.ip_address(ip)
        except ValueError:
            continue
        # Loopback and APIPA (169.254.x.x) never reach a reader.
        if addr.is_loopback or addr.is_link_local:
            continue
        unique.append(ip)
    return unique


def device_credentials(settings: Settings) -> tuple[str, str]:
    """(deviceId, token) a reader should use.

    A shared RELAY_DEVICE_TOKEN wins; otherwise the first entry of the
    per-device map is shown. ALLOWED_DEVICE_IDS may narrow the id list, in
    which case the first allowed id is paired with the shared token.
    """
    allowed = [item.strip() for item in settings.allowed_device_ids.split(",") if item.strip()]
    token = settings.relay_device_token.strip()
    if token:
        return (allowed[0] if allowed else DEFAULT_DEVICE_ID), token
    for pair in settings.relay_device_tokens.split(","):
        if ":" not in pair:
            continue
        device_id, _, per_device_token = pair.partition(":")
        device_id = device_id.strip()
        per_device_token = per_device_token.strip()
        if device_id and per_device_token:
            return device_id, per_device_token
    return (allowed[0] if allowed else DEFAULT_DEVICE_ID), ""


def _status(settings: Settings) -> dict:
    report = settings.ready_report()
    ready = report["asr"] == "configured" and report["llm"] == "configured"
    device_id, token = device_credentials(settings)
    lan_ips = _lan_ips()
    relay_url = settings.relay_public_url.strip()
    if not relay_url:
        host = lan_ips[0] if lan_ips else "<this-host-ip>"
        relay_url = f"ws://{host}:{settings.relay_port}/api/v1/voice/stream"
    return {
        "service": SERVICE_NAME,
        "version": settings.relay_version,
        "relayVersion": __version__,
        "env": settings.relay_env,
        "ready": ready,
        "status": report,
        "bind": settings.relay_bind,
        "port": settings.relay_port,
        "lanIps": lan_ips,
        "relayUrl": relay_url,
        "deviceId": device_id,
        "deviceToken": token,
        "limits": {
            "maxSessions": settings.max_concurrent_sessions,
            "maxSessionsPerDevice": settings.max_sessions_per_device,
            "maxConnections": settings.max_ws_connections,
            "idleTimeoutSeconds": settings.session_idle_timeout_seconds,
        },
    }


ADMIN_HTML = """<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Voice Relay - 部署状态</title>
<style>
  :root { color-scheme: light dark; --fg:#1a1a1a; --bg:#f6f6f4; --card:#fff; --muted:#666; --line:#ddd; --ok:#1a7f37; --warn:#9a6700; --bad:#b42318; }
  @media (prefers-color-scheme: dark) { :root { --fg:#e6e6e3; --bg:#141414; --card:#1e1e1e; --muted:#9a9a96; --line:#333; --ok:#4ade80; --warn:#fbbf24; --bad:#f87171; } }
  * { box-sizing: border-box; }
  body { margin:0; font:15px/1.5 -apple-system, "Segoe UI", "Noto Sans SC", sans-serif; color:var(--fg); background:var(--bg); }
  header { display:flex; align-items:center; gap:12px; padding:20px 24px 0; max-width:760px; margin:0 auto; }
  h1 { font-size:20px; margin:0; flex:1; }
  .badge { font-size:13px; padding:4px 10px; border-radius:999px; border:1px solid var(--line); background:var(--card); }
  .badge.ok { color:var(--ok); border-color:var(--ok); }
  .badge.warn { color:var(--warn); border-color:var(--warn); }
  main { max-width:760px; margin:0 auto; padding:16px 24px 40px; display:grid; gap:14px; }
  .card { background:var(--card); border:1px solid var(--line); border-radius:12px; padding:16px 18px; }
  h2 { font-size:15px; margin:0 0 12px; }
  h2 small { color:var(--muted); font-weight:400; margin-left:6px; }
  label { display:block; margin:10px 0 2px; color:var(--muted); font-size:12px; }
  .row { display:flex; align-items:center; gap:8px; flex-wrap:wrap; }
  code { font:13px/1.6 ui-monospace, "Cascadia Mono", Consolas, monospace; word-break:break-all; flex:1; min-width:0; }
  button { font:13px inherit; padding:5px 12px; border-radius:8px; border:1px solid var(--line); background:transparent; color:var(--fg); cursor:pointer; }
  button:hover { border-color:var(--muted); }
  .chips { display:flex; gap:8px; flex-wrap:wrap; }
  .chip { font-size:12px; padding:3px 9px; border-radius:999px; border:1px solid var(--line); }
  .chip.ok { color:var(--ok); border-color:var(--ok); }
  .chip.warn { color:var(--warn); border-color:var(--warn); }
  .chip.bad { color:var(--bad); border-color:var(--bad); }
  .hint { color:var(--muted); font-size:13px; margin:10px 0 0; }
  .kv { display:grid; grid-template-columns:150px 1fr; gap:4px 12px; font-size:13px; }
  .kv span:nth-child(odd) { color:var(--muted); }
  footer { max-width:760px; margin:0 auto; padding:0 24px 30px; color:var(--muted); font-size:12px; }
  #toast { position:fixed; left:50%; bottom:24px; transform:translateX(-50%); background:var(--fg); color:var(--bg); padding:8px 16px; border-radius:999px; font-size:13px; opacity:0; transition:opacity .2s; pointer-events:none; }
  #toast.show { opacity:1; }
</style>
</head>
<body>
<header>
  <h1>Reading Companion Voice Relay</h1>
  <span id="badge" class="badge">连接中…</span>
</header>
<main>
  <section class="card">
    <h2>设备配置 <small>阅读器 Web UI → 设置 → Voice Relay</small></h2>
    <label>Relay URL</label>
    <div class="row"><code id="relayUrl">…</code><button data-copy="relayUrl">复制</button></div>
    <label>Device ID</label>
    <div class="row"><code id="deviceId">…</code><button data-copy="deviceId">复制</button></div>
    <label>Token</label>
    <div class="row">
      <code id="deviceToken">••••••••••••</code>
      <button id="reveal" type="button">显示</button>
      <button data-copy="tokenValue" type="button">复制</button>
    </div>
    <p class="hint">把以上三项填入阅读器的 Web UI 后保存；设备端 Voice Relay 开关默认开启。</p>
  </section>

  <section class="card">
    <h2>服务状态</h2>
    <div class="chips" id="providers"></div>
    <div class="kv" style="margin-top:12px">
      <span>版本</span><span id="version">…</span>
      <span>运行环境</span><span id="env">…</span>
      <span>监听</span><span id="listen">…</span>
      <span>局域网地址</span><span id="lanIps">…</span>
    </div>
    <p class="hint" id="mockHint" hidden>当前为 mock 模式：语音回合返回固定文本。在配置文件中填入 DashScope API Key 并重启服务即可切换真实语音。</p>
    <p class="hint" id="tlsHint" hidden>设备走 ws:// 明文连接。若经公网使用，请通过 nginx/Caddy 提供 wss:// 并在设备上填写域名地址。</p>
  </section>

  <section class="card">
    <h2>并发限制</h2>
    <div class="chips" id="limits"></div>
    <p class="hint">多台阅读器可共用本中继；每个设备同时只跑一个语音会话。</p>
  </section>
</main>
<footer>本页面仅允许从本机访问（127.0.0.1 / ::1）。Windows 服务：ReadingCompanionVoiceRelay。</footer>
<div id="toast"></div>
<script>
let tokenValue = "";
const $ = (id) => document.getElementById(id);
function chip(cls, text) { const el = document.createElement("span"); el.className = "chip " + cls; el.textContent = text; return el; }
function providerChip(name, value) {
  const cls = value === "configured" || value === "mock" || value === "local" ? "ok" : (value === "missing_config" || value === "local_missing_data" ? "bad" : "warn");
  return chip(cls, name + ": " + value);
}
function toast(text) { const el = $("toast"); el.textContent = text; el.classList.add("show"); setTimeout(() => el.classList.remove("show"), 1400); }
async function copyText(text, label) {
  try { await navigator.clipboard.writeText(text); toast(label + " 已复制"); }
  catch (e) { const ta = document.createElement("textarea"); ta.value = text; document.body.appendChild(ta); ta.select(); document.execCommand("copy"); ta.remove(); toast(label + " 已复制"); }
}
async function refresh() {
  try {
    const res = await fetch("/admin/status", { cache: "no-store" });
    if (!res.ok) throw new Error(String(res.status));
    const s = await res.json();
    $("badge").textContent = s.ready ? "运行中 · 已就绪" : "运行中 · 配置不完整";
    $("badge").className = "badge " + (s.ready ? "ok" : "warn");
    $("relayUrl").textContent = s.relayUrl;
    $("deviceId").textContent = s.deviceId || "(未配置)";
    tokenValue = s.deviceToken || "";
    $("deviceToken").textContent = tokenValue ? "••••••••••••" : "(未配置)";
    $("version").textContent = s.version + " (" + s.relayVersion + ")";
    $("env").textContent = s.env;
    $("listen").textContent = s.bind + ":" + s.port;
    $("lanIps").textContent = (s.lanIps && s.lanIps.length) ? s.lanIps.join(", ") : "未检测到";
    const providers = $("providers"); providers.replaceChildren(
      providerChip("ASR", s.status.asr), providerChip("LLM", s.status.llm),
      providerChip("字典", s.status.dict), providerChip("生图", s.status.t2i));
    $("mockHint").hidden = !(s.status.asr === "mock" || s.status.llm === "mock");
    $("tlsHint").hidden = !s.relayUrl.startsWith("ws://");
    const limits = $("limits"); limits.replaceChildren(
      chip("", "全局会话 " + s.limits.maxSessions),
      chip("", "单设备会话 " + s.limits.maxSessionsPerDevice),
      chip("", "WebSocket 上限 " + s.limits.maxConnections),
      chip("", "空闲回收 " + s.limits.idleTimeoutSeconds + "s"));
  } catch (e) {
    $("badge").textContent = "无法连接";
    $("badge").className = "badge warn";
  }
}
document.addEventListener("click", (ev) => {
  const btn = ev.target.closest("button[data-copy]");
  if (!btn) return;
  const what = btn.getAttribute("data-copy");
  if (what === "tokenValue") { if (tokenValue) copyText(tokenValue, "Token"); else toast("Token 未配置"); return; }
  const el = $(what); if (el) copyText(el.textContent, what === "relayUrl" ? "Relay URL" : "Device ID");
});
$("reveal").addEventListener("click", () => {
  if (!tokenValue) { toast("Token 未配置"); return; }
  const el = $("deviceToken");
  const shown = el.dataset.shown === "1";
  el.textContent = shown ? "••••••••••••" : tokenValue;
  el.dataset.shown = shown ? "0" : "1";
  $("reveal").textContent = shown ? "显示" : "隐藏";
});
refresh();
setInterval(refresh, 5000);
</script>
</body>
</html>
"""


def register_admin_routes(app: FastAPI) -> None:
    @app.get("/admin", response_class=HTMLResponse)
    async def admin_page(request: Request):  # noqa: ANN202 - FastAPI route
        settings = get_settings()
        if not _authorized(request, settings):
            return HTMLResponse(
                "<h1>403</h1><p>This page is only available from the relay host "
                "(127.0.0.1). Use an SSH tunnel or set RELAY_ADMIN_TOKEN.</p>",
                status_code=403,
            )
        return HTMLResponse(ADMIN_HTML)

    @app.get("/admin/status")
    async def admin_status(request: Request):
        settings = get_settings()
        if not _authorized(request, settings):
            return JSONResponse(status_code=403, content={"error": "forbidden"})
        return JSONResponse(content=_status(settings))
