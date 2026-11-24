"""startup-ready follows the configured topology, not which daemons are alive.

The init script is run for real with overridable runtime directories.  Each
case starts the daemon, publishes the pidfiles and sockets the selected
topology owns, and checks whether the readiness marker appears.
"""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
INIT = ROOT / "init/libreecho-web.init"

BASE_SERVICES = ("networkd", "timed", "audiod", "micd", "ledd", "web")
BASE_SOCKETS = ("network", "audio", "mic", "led")
VOICE_SERVICES = ("sttd", "ttsd", "agentd")
VOICE_SOCKETS = ("stt", "tts", "agent")

FAKE_DAEMON = """#!/bin/sh
trap 'exit 0' TERM INT
while :; do sleep 1; done
"""
FAKE_ESPHOMED = """#!/bin/sh
exit "$FAKE_ESPHOMED_STATUS"
"""


class StartupReadinessTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.var_run = self.root / "var/run"
        self.socket_dir = self.root / "run"
        self.var_run.mkdir(parents=True)
        self.socket_dir.mkdir(parents=True)
        self.marker = self.root / "startup-ready"
        self.pidfile = self.var_run / "libreecho-web.pid"
        self.config = self.root / "web-config.json"
        self.policy = self.root / "feature-policy"
        self.policy.write_text("redistributable\n")
        self.daemon = self.root / "fake-web"
        self.daemon.write_text(FAKE_DAEMON)
        self.daemon.chmod(0o755)
        self.esphomed = self.root / "fake-esphomed"
        self.esphomed.write_text(FAKE_ESPHOMED)
        self.esphomed.chmod(0o755)
        self.holders = []
        self.process = None

    def tearDown(self) -> None:
        if self.process is not None:
            try:
                os.killpg(self.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.process.wait()
        for holder in self.holders:
            holder.close()
        if self.pidfile.exists():
            try:
                pid = int(self.pidfile.read_text().split()[0])
                if pid != os.getpid():
                    os.kill(pid, signal.SIGKILL)
            except (ProcessLookupError, ValueError, IndexError):
                pass
        self.temporary.cleanup()

    def publish(self, services, sockets) -> None:
        for name in (*BASE_SERVICES, *services):
            (self.var_run / f"libreecho-{name}.pid").write_text(
                f"{os.getpid()}\n")
        for name in (*BASE_SOCKETS, *sockets):
            holder = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            holder.bind(str(self.socket_dir / f"{name}.sock"))
            self.holders.append(holder)

    def start(self, integrations, esphomed_status=0) -> None:
        self.config.write_text(json.dumps({"integrations": integrations}))
        environment = {
            **os.environ,
            "VAR_RUN": str(self.var_run),
            "SOCKET_DIR": str(self.socket_dir),
            "STARTUP_READY": str(self.marker),
            "PIDFILE": str(self.pidfile),
            "LOGFILE": str(self.root / "log"),
            "CONFIG_FILE": str(self.config),
            "DEFAULT_CONFIG_FILE": str(self.root / "missing-default"),
            "BT_READY_PATH": str(self.root / "bluetooth-ready"),
            "FEATURE_POLICY_FILE": str(self.policy),
            "DAEMON": str(self.daemon),
            "LIBREECHO_ESPHOMED_DAEMON": str(self.esphomed),
            "LIBREECHO_ESPHOMED_PIDFILE": str(self.root / "esphomed.pid"),
            "LIBREECHO_ESPHOME_STATUS_FILE": str(self.root / "esphome-status.json"),
            "FAKE_ESPHOMED_STATUS": str(esphomed_status),
        }
        self.process = subprocess.Popen(
            ["/bin/sh", str(INIT), "start"],
            env=environment,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )

    def wait_for_marker(self, expected, label) -> None:
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if self.marker.exists():
                break
            time.sleep(0.05)
        self.assertEqual(self.marker.exists(), expected, label)

    def test_local_topology_waits_for_the_whole_assistant(self) -> None:
        self.publish(VOICE_SERVICES, VOICE_SOCKETS)
        self.start(integrations=0)
        self.wait_for_marker(True, "local assistant ready")

    def test_local_topology_holds_until_agentd_is_up(self) -> None:
        # The reconciler starts sttd and agentd after web, so ttsd alone is the
        # normal intermediate state and must not publish the marker early.
        self.publish(("sttd", "ttsd"), ("stt", "tts"))
        self.start(integrations=0)
        self.wait_for_marker(False, "local assistant incomplete")

    def test_home_assistant_topology_ignores_voice_daemons(self) -> None:
        # No voice pidfiles at all: the HA topology runs only esphomed.
        self.publish((), ())
        self.start(integrations=1)
        self.wait_for_marker(True, "home assistant ready")

    def test_home_assistant_topology_waits_for_esphomed(self) -> None:
        self.publish((), ())
        self.start(integrations=1, esphomed_status=1)
        self.wait_for_marker(False, "home assistant esphomed unhealthy")

    def test_home_assistant_topology_does_not_accept_voice_liveness(self) -> None:
        # Voice daemons being alive is not what HA mode needs: a failing
        # esphomed must keep the gate closed.
        self.publish(VOICE_SERVICES, VOICE_SOCKETS)
        self.start(integrations=1, esphomed_status=1)
        self.wait_for_marker(False, "home assistant with stale voice stack")


if __name__ == "__main__":
    unittest.main()
