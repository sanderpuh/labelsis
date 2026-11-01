
import os
import sys
import subprocess
from pathlib import Path
from SCons.Script import COMMAND_LINE_TARGETS

Import("env")

if not env.IsIntegrationDump() and "clean" not in COMMAND_LINE_TARGETS:

    platform = env.PioPlatform()

    def package(name):
        path = platform.get_package_dir(name)
        if not path:
            raise RuntimeError(f"Missing PlatformIO package: {name}")
        return Path(path)

    build_dir = Path(env.subst("$BUILD_DIR")).resolve()

    cmake = package("tool-cmake") / "bin" / (
        "cmake.exe" if os.name == "nt" else "cmake"
    )

    if not (build_dir / "build.ninja").exists():
        raise RuntimeError("ESP-IDF CMake build not configured")

    # Build the environment expected by ESP-IDF/CMake
    idf_env = os.environ.copy()

    idf_env["IDF_PATH"] = package("framework-espidf").as_posix()

    tool_paths = [
        package("toolchain-xtensa-esp-elf") / "bin",
        package("tool-ninja"),
        cmake.parent,
        Path(sys.executable).parent,
        package("toolchain-esp32ulp") / "bin",
    ]

    if os.name == "nt":
        tool_paths.append(package("tool-mconf"))

    idf_env["PATH"] = os.pathsep.join(
        [str(p) for p in tool_paths]
        + [idf_env.get("PATH", "")]
    )

    idf_env["ESP_ROM_ELF_DIR"] = str(
        package("tool-esp-rom-elfs")
    )
    idf_env["IDF_COMPONENT_OVERWRITE_MANAGED_COMPONENTS"] = "1"
    idf_env.pop("IDF_TOOLS_PATH", None)

    print("Preparing LabelSis web assets with ESP-IDF environment...")

    subprocess.run(
        [
            str(cmake),
            "--build",
            str(build_dir),
            "--target",
            "__idf_pt_app",
        ],
        env=idf_env,
        check=True,
    )
