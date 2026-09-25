import io
import importlib
import os
import subprocess
import sys
import unittest
import zipfile
from contextlib import redirect_stdout


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "packaging"))

make_release = importlib.import_module("make_release")


class MakeReleaseEncodingTest(unittest.TestCase):
    def test_configures_utf8_output_for_non_ascii_status_messages(self):
        configure_output_encoding = getattr(
            make_release, "configure_output_encoding", None
        )
        self.assertIsNotNone(configure_output_encoding)

        raw = io.BytesIO()
        stream = io.TextIOWrapper(raw, encoding="cp1252")

        with redirect_stdout(stream):
            configure_output_encoding()
            print("打包完成")
            stream.flush()

        self.assertEqual(raw.getvalue().decode("utf-8"), "打包完成" + os.linesep)

    def test_package_contains_radar_extension_files(self):
        script = os.path.join(ROOT, "packaging", "make_release.py")
        env = os.environ.copy()
        env["PYTHONIOENCODING"] = "cp1252"
        subprocess.run([sys.executable, script], cwd=ROOT, env=env, check=True)
        archives = sorted([
            name for name in os.listdir(os.path.join(ROOT, "dist"))
            if name.endswith("-win64.zip")
        ], reverse=True)
        self.assertTrue(archives)
        with zipfile.ZipFile(os.path.join(ROOT, "dist", archives[0])) as archive:
            names = set(archive.namelist())
        prefix = archives[0][:-4]
        expected = {
            f"{prefix}/extension/popup.html",
            f"{prefix}/extension/popup.css",
            f"{prefix}/extension/popup.js",
            f"{prefix}/extension/content.js",
            f"{prefix}/extension/icons/swoop.svg",
        }
        self.assertTrue(expected.issubset(names), sorted(expected - names))


if __name__ == "__main__":
    unittest.main()
