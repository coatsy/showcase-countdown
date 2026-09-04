#!/usr/bin/env python3
"""Compile the firmware's light schedule logic and its static assertions."""

import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def find_compiler():
    for name in ("g++", "clang++"):
        compiler = shutil.which(name)
        if compiler:
            return Path(compiler)

    platformio_compiler = (
        Path.home()
        / ".platformio"
        / "packages"
        / "toolchain-xtensa-esp32"
        / "bin"
        / ("xtensa-esp32-elf-g++.exe" if os.name == "nt" else "xtensa-esp32-elf-g++")
    )
    return platformio_compiler if platformio_compiler.is_file() else None


def main():
    compiler = find_compiler()
    if compiler is None:
        print("no C++ compiler found - install PlatformIO, g++, or clang++")
        return 1

    source = ROOT / "tests" / "light_schedule_compile.cpp"
    subprocess.run(
        [
            str(compiler),
            "-std=c++11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsyntax-only",
            "-I",
            str(ROOT / "src"),
            str(source),
        ],
        check=True,
    )
    print("light schedule boundaries, offsets, and overnight window passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())