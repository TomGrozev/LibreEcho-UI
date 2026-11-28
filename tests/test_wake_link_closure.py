#!/usr/bin/env python3
"""Exercise the shipped microWakeWord link recipe with the TFLM archive layout.

This is a native linker regression, not an ARM32 inference test. Only the
three target-architecture flags are translated; the production recipe, archive
selection, static linking and unresolved-symbol checks execute unchanged.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WakeLinkClosure(unittest.TestCase):
    def command(self, args, **kwargs):
        return subprocess.run(args, cwd=ROOT, text=True, capture_output=True,
                              timeout=20, **kwargs)

    def test_pinned_layout_links_and_required_archive_remains_mandatory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            speex = root / "speex"
            speex_archive = speex / "lib/libspeexdsp.a"
            speex_archive.parent.mkdir(parents=True)
            self.command(["ar", "rcs", str(speex_archive)], check=True)
            tflm = root / "libtflm-micro.a"
            self.command(["ar", "rcs", str(tflm)], check=True)
            # A real unresolved reference must be satisfied from the frontend
            # archive, so deleting it later must break the link.
            main = root / "main.c"
            main.write_text("int dependency_probe(void);\n"
                            "int main(void) { return dependency_probe() != 42; }\n")
            dependency = root / "dependency.c"
            dependency.write_text("int dependency_probe(void) { return 42; }\n")
            obj, dep_obj = root / "main.o", root / "dependency.o"
            for source, output in ((main, obj), (dependency, dep_obj)):
                self.command(["cc", "-c", str(source), "-o", str(output)], check=True)
            required = root / "libmicrowakeword-frontend.a"
            self.command(["ar", "rcs", str(required), str(dep_obj)], check=True)
            compiler = shutil.which("g++")
            self.assertIsNotNone(compiler)
            wrapper = root / "native-g++"
            wrapper.write_text("#!/usr/bin/env python3\nimport os, sys\n"
                               "flags = ('-march=', '-mfpu=', '-mfloat-abi=')\n"
                               f"os.execv({compiler!r}, [{compiler!r}] + "
                               "[a for a in sys.argv[1:] if not a.startswith(flags)])\n")
            wrapper.chmod(0o755)
            output = root / "build/libreecho-waked-mww-arm32"
            output.parent.mkdir()
            args = ["make", "--no-print-directory", "-s", str(output),
                    f"BUILD={output.parent}", f"CROSS_COMPILE={root}/native-",
                    f"WAKE_DAEMON_MWW_ARM_OBJECTS={obj}",
                    f"WAKE_TFLM_LIB={tflm}",
                    f"WAKE_TFLM_FRONTEND_LIB={required}",
                    f"ARM_SPEEX_PREFIX={speex}"]
            result = self.command(args)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.command([str(output)], check=True)
            output.unlink()
            required.unlink()
            result = self.command(args)
            self.assertNotEqual(result.returncode, 0, "missing required archive accepted")
            self.assertIn("libmicrowakeword-frontend.a", result.stderr)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
