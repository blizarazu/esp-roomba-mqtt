"""
PlatformIO pre-build script: inject FIRMWARE_VERSION as a C preprocessor macro.

Version format: MAJOR.MINOR.PATCH
  MAJOR / MINOR — bump manually here when making breaking or significant changes.
  PATCH         — total git commit count, increments automatically with every commit.

If git is unavailable the version falls back to MAJOR.MINOR.0+nogit.
"""

Import("env")  # noqa: F821  (PlatformIO injects this)
import subprocess

MAJOR = 1
MINOR = 0

try:
    patch = int(subprocess.check_output(
        ["git", "rev-list", "--count", "HEAD"],
        stderr=subprocess.DEVNULL,
        cwd=env["PROJECT_DIR"],  # noqa: F821
    ).decode().strip())
    version = "{}.{}.{}".format(MAJOR, MINOR, patch)
except Exception:
    version = "{}.{}.0+nogit".format(MAJOR, MINOR)

print("Firmware version: {}".format(version))
env.Append(CPPDEFINES=[("FIRMWARE_VERSION", env.StringifyMacro(version))])  # noqa: F821
