import io
import importlib
import os
import sys
import unittest
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


if __name__ == "__main__":
    unittest.main()
