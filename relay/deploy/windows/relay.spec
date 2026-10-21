# PyInstaller spec for the Windows relay payload (onedir).
#
# Build from the relay/ root:
#   pyinstaller --clean --noconfirm --distpath build/windows/pyinstaller \
#               --workpath build/windows/work deploy/windows/relay.spec
#
# dict-data/ is copied next to voice-relay.exe by build.ps1 (not through
# datas) so the runtime path stays trivial: <install>\voice-relay\dict-data.

from pathlib import Path

from PyInstaller.utils.hooks import collect_submodules

# SPECPATH is <repo>/relay/deploy/windows; parents[1] is <repo>/relay, the
# directory that contains the `relay/` package and requirements.txt.
RELAY_ROOT = Path(SPECPATH).resolve().parents[1]

hiddenimports = (
    collect_submodules("uvicorn")
    + collect_submodules("websockets")
    + collect_submodules("pydantic")
    + [
        "anyio",
        "fastapi",
        "httptools",
        "httpx",
        "orjson",
        "prometheus_client",
        "pythonjsonlogger",
        "starlette",
    ]
)

a = Analysis(
    [str(RELAY_ROOT / "deploy" / "windows" / "entry.py")],
    pathex=[str(RELAY_ROOT)],
    binaries=[],
    datas=[],
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=["pytest", "tkinter"],
    noarchive=False,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="voice-relay",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=True,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)

coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=False,
    name="voice-relay",
)
