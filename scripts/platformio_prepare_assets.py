
import os
import subprocess
from pathlib import Path

from SCons.Script import COMMAND_LINE_TARGETS

Import("env")

if not env.IsIntegrationDump() and "clean" not in COMMAND_LINE_TARGETS:
    build_dir = Path(env.subst("$BUILD_DIR")).resolve()

    cmake = (
        Path(env.PioPlatform().get_package_dir("tool-cmake"))
        / "bin"
        / ("cmake.exe" if os.name == "nt" else "cmake")
    )

    if not (build_dir / "build.ninja").exists():
        raise RuntimeError("ESP-IDF CMake build not configured")

    print("Preparing LabelSis web assets with CMake...")

    subprocess.run(
        [
            str(cmake),
            "--build",
            str(build_dir),
            "--target",
            "__idf_pt_app",
        ],
        check=True,
    )
