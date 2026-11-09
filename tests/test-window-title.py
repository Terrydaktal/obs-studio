"""Exercise the maintained title method in an offscreen Qt window, not OBS."""

import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class WindowTitleTests(unittest.TestCase):
    def test_main_window_title_is_obs_after_every_refresh(self):
        if not shutil.which("c++") or not shutil.which("pkg-config"):
            self.skipTest("A C++ compiler and Qt Widgets development files are needed")
        flags = subprocess.run(
            ["pkg-config", "--cflags", "--libs", "Qt6Widgets"],
            capture_output=True,
            text=True,
            timeout=15,
        )
        if flags.returncode:
            self.skipTest("Qt Widgets development files are needed")
        source = (ROOT / "frontend/widgets/OBSBasic.cpp").read_text()
        method = re.search(
            r"^void OBSBasic::UpdateTitleBar\(\)\n\{.*?^\}", source, re.M | re.S
        )
        self.assertIsNotNone(method)
        probe = (
            "#include <QApplication>\n#include <QMainWindow>\n"
            "class OBSBasic : public QMainWindow { public: void UpdateTitleBar(); };\n"
            + method.group()
            + "\nint main(int argc, char **argv) {\n"
            "  QApplication app(argc, argv); OBSBasic window;\n"
            '  for (const char *old : {"", "OBS 32.2.1 - Profile: Untitled", "OBS Studio"}) {\n'
            "    window.setWindowTitle(QString::fromUtf8(old));\n"
            "    window.UpdateTitleBar(); window.UpdateTitleBar();\n"
            '    if (window.windowTitle() != QStringLiteral("OBS")) return 1;\n'
            "  }\n  return 0;\n}\n"
        )
        with tempfile.TemporaryDirectory(prefix="obs-title-test-") as temporary:
            directory = Path(temporary)
            cpp = directory / "probe.cpp"
            binary = directory / "probe"
            cpp.write_text(probe)
            compiled = subprocess.run(
                [
                    "c++",
                    "-std=c++17",
                    "-fPIC",
                    str(cpp),
                    "-o",
                    str(binary),
                    *shlex.split(flags.stdout),
                ],
                capture_output=True,
                text=True,
                timeout=45,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            environment = os.environ.copy()
            environment.update(
                QT_QPA_PLATFORM="offscreen", XDG_CONFIG_HOME=str(directory)
            )
            result = subprocess.run(
                [str(binary)],
                env=environment,
                capture_output=True,
                text=True,
                timeout=15,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
