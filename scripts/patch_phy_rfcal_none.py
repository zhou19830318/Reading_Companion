"""
PlatformIO pre-build script: stop a failed PHY calibration-data load from
escalating into the full-calibration crash on this ESP32-C61 (eco4).

Background
----------
The arduino-esp32 C61 framework ships `esp_phy` as a prebuilt archive
(`framework-arduinoespressif32-libs/esp32c61/lib/libesp_phy.a`), so our
`CONFIG_ESP_PHY_RF_CAL_NONE=y` in platformio.ini/sdkconfig NEVER reaches it —
that Kconfig only matters when phy_init.c is compiled from source. The
prebuilt `esp_phy_load_cal_and_init` computes the calibration mode as
(mode enum: PARTIAL=0, NONE=1, FULL=2; 1904-byte cal buffer):

    variant A (.o-era member):  mode = (nvs_load_ok) ? PHY_RF_CAL_NONE(1)
                                                     : PHY_RF_CAL_FULL(2)
    variant B (.obj member):    mode = (nvs_load_ok) ? ((mac_check==5) ? NONE(1)
                                                                      : PARTIAL(0))
                                                     : PHY_RF_CAL_FULL(2)

i.e. normal boots never calibrate, but ANY NVS load failure (wiped NVS, phy
version/CRC mismatch after a shared-package flip, ...) escalates to FULL —
and full calibration crashes on this eco4 ROM in `phy_iq_est_enable_new` /
`phy_dc_iq_est_new` (espressif/esp-idf#15424; reproduced on-device 2026-09-30
10:46/10:47/10:54 and 16:23, MEPC/MTVAL match the platformio.ini analysis).
A crashing FULL cal can never store fresh data, so the device then
WiFi-crash-loops.

Fix
---
Patch archive member `phy_init.c.o`/`phy_init.c.obj` (relocatable code, the
patch sites involve no relocations) so a load failure keeps NONE:

  Variant A (10-byte site in .text.esp_phy_load_cal_and_init):
    snez s3,a0 ; mv s2,a0 ; addi a0,sp,8 ; addi s3,s3,1   (err ? FULL : NONE)
    -> li s3,1    ; mv s2,a0 ; addi a0,sp,8 ; c.nop        (always NONE)
    bytes b3 39 a0 00 | 2a 89 | 28 00 | 85 09  ->  93 09 10 00 | 2a 89 | 28 00 | 01 00

  Variant B (12-byte site, c.li s4,2 -> c.li s4,1):
    mv s3,a0 ; c.li s4,2 ; bnez a0,+8 ; addi s2,-5 ; seqz s4,s2
    -> mv s3,a0 ; c.li s4,1 ; ...
    bytes aa 89 09 4a 01 e5 6d 19 13 3a 19 00  ->  ... 05 4a ...
    (the success path overwrites s4 via seqz, so only the failure path
    changes: FULL(2) -> NONE(1))

This matches what every healthy boot already runs (no startup calibration);
the only difference is the failure path now runs NONE with a zeroed cal
buffer instead of FULL (which crashes). TX power stays capped by
CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=20.

House rules (compare patch_ld_sleep_clock.py / patch_gthr_static_mutex.py):
- Idempotent: detects an already-patched member and exits quietly.
- Refuses to act if neither site pattern is present exactly once (framework
  update restructured the function) — warns and lets the build continue
  unpatched instead of corrupting the archive.
- NEVER aborts the build: every failure is a warning + skip (a missing
  patch means the known WiFi crash may return; a hard abort blocks all work).
- Re-runs every build because the shared ~/.platformio package cache can be
  re-extracted under us at any time (sibling projects use the same cache;
  .piopm was observed flipping at 17:05 while this project was idle).
"""

Import("env")  # noqa: F821 (SCons-injected global)

import os
import subprocess
import tempfile

LIB_REL = os.path.join("esp32c61", "lib", "libesp_phy.a")

# Patch sites (see header). 32-bit words are little-endian byte strings.
SITE_A_OLD = bytes.fromhex("b3 39 a0 00 2a 89 28 00 85 09")
SITE_A_MIS = bytes.fromhex("93 03 10 00 2a 89 28 00 01 00")  # rd=7 (t2), briefly shipped
SITE_A_GOOD = bytes.fromhex("93 09 10 00 2a 89 28 00 01 00")
SITE_B_OLD = bytes.fromhex("aa 89 09 4a 01 e5 6d 19 13 3a 19 00")
SITE_B_GOOD = bytes.fromhex("aa 89 05 4a 01 e5 6d 19 13 3a 19 00")


def _warn(msg):
    print("WARNING: patch_phy_rfcal_none: %s" % msg)


def _find_member(archive_path):
    """Return the phy_init member name (`.o`/`.obj`/other revisions)."""
    listing = subprocess.run(
        ["ar", "t", archive_path], capture_output=True, text=True, check=False
    )
    if listing.returncode != 0:
        return None
    for name in listing.stdout.splitlines():
        if name.startswith("phy_init.c."):
            return name
    return None


def patch_phy_rfcal_none(env):  # noqa: C901 (flat linear guard chain)
    try:
        pkg_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")
        if not pkg_dir:
            return  # not the C61-from-source build; nothing to patch
        lib_path = os.path.join(pkg_dir, LIB_REL)
        if not os.path.isfile(lib_path):
            _warn("archive not found, skipping: %s" % lib_path)
            return

        member = _find_member(lib_path)
        if not member:
            _warn("no phy_init.c.* member in archive; layout changed, skipping")
            return

        with tempfile.TemporaryDirectory() as td:
            extract = subprocess.run(
                ["ar", "x", lib_path, member], cwd=td, capture_output=True, check=False
            )
            obj_path = os.path.join(td, member)
            if extract.returncode != 0 or not os.path.isfile(obj_path):
                # GNU ar can exit 0 while reporting "no entry" — file check matters.
                _warn("extraction of %s failed; skipping" % member)
                return

            with open(obj_path, "rb") as f:
                data = f.read()

            counts = {
                "A_old": data.count(SITE_A_OLD),
                "A_mis": data.count(SITE_A_MIS),
                "A_good": data.count(SITE_A_GOOD),
                "B_old": data.count(SITE_B_OLD),
                "B_good": data.count(SITE_B_GOOD),
            }
            if counts["A_good"] == 1 and counts["A_old"] == 0 and counts["A_mis"] == 0:
                return  # variant A already patched (idempotent fast path)
            if counts["B_good"] == 1 and counts["B_old"] == 0:
                return  # variant B already patched

            if counts["A_old"] + counts["A_mis"] == 1 and counts["B_old"] == 0:
                src, dst = (SITE_A_OLD if counts["A_old"] else SITE_A_MIS), SITE_A_GOOD
            elif counts["B_old"] == 1 and counts["A_old"] == 0 and counts["A_mis"] == 0:
                src, dst = SITE_B_OLD, SITE_B_GOOD
            else:
                # Neither layout recognised — do not risk corrupting the archive.
                # Build proceeds unpatched; a WiFi bring-up with broken NVS cal
                # data will still crash, which is the correct signal to revisit.
                _warn(
                    "no unique patch site in %s (counts=%s); upstream layout "
                    "changed, skipping. WiFi may crash-loop on NVS cal-load "
                    "failure until revisited." % (member, counts)
                )
                return

            patched = data.replace(src, dst, 1)
            with open(obj_path, "wb") as f:
                f.write(patched)

            replace = subprocess.run(
                ["ar", "r", lib_path, obj_path], capture_output=True, check=False
            )
            if replace.returncode != 0:
                _warn("ar r failed (%s)" % replace.stderr.decode("utf-8", "replace").strip())
                return
            print(
                "patch_phy_rfcal_none: patched %s member %s"
                % (os.path.basename(lib_path), member)
            )
    except Exception as exc:  # a patch script must never break the build
        _warn("unexpected error, skipping: %r" % exc)


patch_phy_rfcal_none(env)  # noqa: F821 (SCons-injected global)
