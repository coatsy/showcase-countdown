"""Build-time transport checks; fixture credentials are deliberately not deployable."""

import contextlib
import io
import os
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch

from test_espnow_relay import BuildEnv, ROOT


class MqttConfigTest(unittest.TestCase):
    def render(self, settings):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "mqtt.env"
            config.write_text(
                'EVENT_NAME="Test"\nEVENT_DATETIME="2026-09-17T14:00:00+10:00"\n' + settings
            )
            with patch.dict(os.environ, SHOWCASE_ENV_FILE=str(config)), contextlib.redirect_stdout(io.StringIO()):
                runpy.run_path(
                    str(ROOT / "scripts" / "load_env.py"),
                    init_globals={"env": BuildEnv(directory), "Import": lambda _: None},
                )
            return (Path(directory) / "generated/env_config.h").read_text()

    def test_disabled_and_legacy_lan(self):
        self.assertIn('#define MQTT_URI ""', self.render(""))
        header = self.render('MQTT_HOST="192.168.8.1"\nMQTT_PORT=1883\n')
        self.assertIn('#define MQTT_URI "mqtt://192.168.8.1:1883"', header)
        self.assertIn("#define MQTT_TLS 0", header)

    def test_authenticated_wss_overrides_legacy_host(self):
        header = self.render(
            'MQTT_HOST="192.168.8.1"\nMQTT_URI="wss://mqtt.cauldnz.org/mqtt"\n'
            'MQTT_USERNAME=52f940\nMQTT_PASSWORD=' + "1" * 32 + "\n"
        )
        self.assertIn('#define MQTT_URI "wss://mqtt.cauldnz.org/mqtt"', header)
        self.assertIn("#define MQTT_TLS 1", header)
        self.assertIn('#define MQTT_PASSWORD "' + "1" * 32 + '"', header)

    def test_invalid_or_unencrypted_credentials_never_echo_secrets(self):
        for uri in (
            "ws://mqtt.example/mqtt", "http://mqtt.example", "wss://",
            "wss://name:DO_NOT_LOG@mqtt.example/mqtt", "wss://mqtt.example:70000/mqtt",
            "wss://mqtt.example/mqtt?password=DO_NOT_LOG", "mqtt://mqtt.example",
            "wss://mqtt.example/mqtt#fragment", "wss://mqtt.example:0/mqtt",
        ):
            with self.subTest(uri=uri), contextlib.redirect_stderr(io.StringIO()) as error:
                with self.assertRaises(SystemExit):
                    self.render(
                        f'MQTT_URI="{uri}"\nMQTT_USERNAME="52f940"\n'
                        'MQTT_PASSWORD="DO_NOT_LOG"\n'
                    )
                self.assertNotIn("DO_NOT_LOG", error.getvalue())

    def test_wss_requires_credentials(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.render('MQTT_URI="wss://mqtt.cauldnz.org/mqtt"\n')


if __name__ == "__main__":
    unittest.main()
