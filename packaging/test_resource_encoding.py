from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ResourceEncodingTest(unittest.TestCase):
    def test_resource_script_declares_utf8(self):
        text = (ROOT / "src" / "gui" / "resources.rc").read_text(encoding="utf-8")
        self.assertIn("#pragma code_page(65001)", text)

    def test_makefile_passes_utf8_codepage_to_windres(self):
        text = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("--codepage=65001", text)

    def test_native_host_supports_show_action(self):
        text = (ROOT / "src" / "nmhost.c").read_text(encoding="utf-8")
        self.assertIn('strcmp(action, "show")', text)


if __name__ == "__main__":
    unittest.main()
