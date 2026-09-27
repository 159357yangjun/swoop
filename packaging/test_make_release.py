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

    def _build_and_read_archive(self):
        script = os.path.join(ROOT, "packaging", "make_release.py")
        env = os.environ.copy()
        env["PYTHONIOENCODING"] = "cp1252"
        subprocess.run([sys.executable, script], cwd=ROOT, env=env, check=True)
        archives = sorted(
            [
                name
                for name in os.listdir(os.path.join(ROOT, "dist"))
                if name.endswith("-win64.zip")
            ],
            reverse=True,
        )
        self.assertTrue(archives)
        archive_name = archives[0]
        with zipfile.ZipFile(os.path.join(ROOT, "dist", archive_name)) as archive:
            names = set(archive.namelist())
        return archive_name[:-4], names

    def test_package_contains_radar_extension_files(self):
        prefix, names = self._build_and_read_archive()
        expected = {
            f"{prefix}/extension/manifest.json",
            f"{prefix}/extension/popup.html",
            f"{prefix}/extension/popup.css",
            f"{prefix}/extension/popup.js",
            f"{prefix}/extension/content.js",
            f"{prefix}/extension/icons/swoop.svg",
        }
        self.assertTrue(expected.issubset(names), sorted(expected - names))

    def test_package_excludes_extension_test_and_cache_files(self):
        _, names = self._build_and_read_archive()
        unexpected = sorted(
            name
            for name in names
            if "/extension/" in name
            and (
                os.path.basename(name).lower().startswith("test_")
                or name.lower().endswith((".py", ".pyc", ".pyo"))
                or "/__pycache__/" in name
                or "/.pytest_cache/" in name
                or "/node_modules/" in name
            )
        )
        self.assertEqual(unexpected, [])


if __name__ == "__main__":
    unittest.main()
