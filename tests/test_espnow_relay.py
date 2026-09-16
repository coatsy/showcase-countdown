"""Host checks for the hardware-independent relay and private config loader."""
import configparser
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BuildEnv:
    def __init__(self, build):
        self.build = str(build)

    def subst(self, value):
        return {"$PROJECT_DIR": str(ROOT), "$BUILD_DIR": self.build}[value]

    def Append(self, **kwargs):
        pass

    def Exit(self, code):
        raise SystemExit(code)


class RelayTest(unittest.TestCase):
    def test_relay_keeps_app_only_partition_layout(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / "platformio.ini")
        self.assertEqual(config["env:stick"]["board_build.partitions"], "min_spiffs.csv")
        self.assertEqual(config["env:stick-relay"]["extends"], "env:stick")
        self.assertEqual(config["env:stick-relay"]["board_build.partitions"], "huge_app.csv")
        self.assertEqual(config["env:bridge"]["board_build.partitions"], "huge_app.csv")

    def test_core(self):
        compiler = shutil.which("g++") or r"C:\Strawberry\c\bin\g++.exe"
        with tempfile.TemporaryDirectory() as temp:
            exe = Path(temp) / ("relay.exe" if os.name == "nt" else "relay")
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "tests" / "relay_core_test.cpp"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_private_config(self):
        with tempfile.TemporaryDirectory() as temp:
            config = Path(temp) / "candidate.env"
            base = 'EVENT_NAME="Test"\nEVENT_DATETIME="2026-09-17T14:00:00+10:00"\n'
            previous = os.environ.get("SHOWCASE_ENV_FILE")
            os.environ["SHOWCASE_ENV_FILE"] = str(config)
            try:
                env = BuildEnv(temp)
                for enabled, key, valid in [
                    ("false", "", True), ("true", "ab" * 32, True),
                    ("true", "", False), ("true", "z" * 64, False),
                ]:
                    config.write_text(base + f'ESPNOW_RELAY_ENABLED={enabled}\nESPNOW_RELAY_KEY="{key}"\n')
                    load = lambda: runpy.run_path(str(ROOT / "scripts" / "load_env.py"),
                                                init_globals={"env": env, "Import": lambda _: None})
                    if not valid:
                        with self.assertRaises(SystemExit):
                            load()
                    else:
                        load()
                        header = (Path(temp) / "generated" / "env_config.h").read_text()
                        self.assertIn("#define EVENT_EPOCH_UTC 1789617600LL", header)
                        self.assertIn(f'#define ESPNOW_RELAY_KEY "{key}"', header)
            finally:
                if previous is None:
                    os.environ.pop("SHOWCASE_ENV_FILE", None)
                else:
                    os.environ["SHOWCASE_ENV_FILE"] = previous


if __name__ == "__main__":
    unittest.main()
