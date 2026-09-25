import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parent


class ExtensionContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = json.loads((ROOT / "manifest.json").read_text(encoding="utf-8"))

    def test_manifest_declares_radar_popup_and_content_script(self):
        self.assertIn("default_popup", self.manifest["action"])
        self.assertEqual(self.manifest["action"]["default_popup"], "popup.html")
        scripts = self.manifest["content_scripts"]
        self.assertTrue(any("content.js" in item["js"] for item in scripts))
        self.assertIn("<all_urls>", scripts[0]["matches"])

    def test_manifest_references_existing_extension_files(self):
        expected = {
            "manifest.json",
            "background.js",
            "popup.html",
            "popup.css",
            "popup.js",
            "content.js",
            "icons/swoop.svg",
        }
        for relative in expected:
            self.assertTrue((ROOT / relative).is_file(), relative)


if __name__ == "__main__":
    unittest.main()
