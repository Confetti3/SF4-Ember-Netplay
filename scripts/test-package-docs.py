import importlib.util
from pathlib import Path
import tempfile
import unittest
import re

spec = importlib.util.spec_from_file_location('package_docs', Path(__file__).with_name('package-docs.py'))
docs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(docs)


class PackageDocsTest(unittest.TestCase):
    def test_actual_inventory_documentation(self):
        repo = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary).resolve()
            inventory = (repo / 'src/common/PackageInventory.inc').read_text()
            names = re.findall(r'^SF4E_PACKAGE_(?:REQUIRED|OPTIONAL)\("([^"]+\.md)"\)', inventory, re.M)
            for name in names:
                relative = Path(name.replace('\\\\', '/'))
                target = package / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.touch()
            docs.relocate(repo, package, 'a' * 40)
            self.assertIn('(docs/TROUBLESHOOTING.md)', (package / 'START_HERE.md').read_text())
            self.assertIn('(SAVING_LOGS.md)', (package / 'docs/TROUBLESHOOTING.md').read_text())

    def test_relocation_offline_guides_source_links_and_anchors(self):
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary) / 'repo'
            package = Path(temporary) / 'package'
            (repo / 'docs/guides').mkdir(parents=True)
            (repo / 'src').mkdir()
            (package / 'docs').mkdir(parents=True)
            (repo / 'src/example.py').write_text('pass')
            (repo / 'docs/guides/USER_NETPLAY.md').write_text('[Help](TROUBLESHOOTING.md#logs)\n[Code](../../src/example.py)\n[Web](https://example.com)\n[Local](#heading)\n[ref]: TROUBLESHOOTING.md\n')
            (repo / 'docs/guides/TROUBLESHOOTING.md').write_text('[Start](USER_NETPLAY.md)')
            (package / 'START_HERE.md').touch()
            (package / 'docs/TROUBLESHOOTING.md').touch()
            self.assertEqual(docs.relocate(repo.resolve(), package.resolve(), 'a' * 40), 2)
            start = (package / 'START_HERE.md').read_text()
            self.assertIn('(docs/TROUBLESHOOTING.md#logs)', start)
            self.assertIn('/blob/' + 'a' * 40 + '/src/example.py', start)
            self.assertIn('[ref]: docs/TROUBLESHOOTING.md', start)
            self.assertIn('(../START_HERE.md)', (package / 'docs/TROUBLESHOOTING.md').read_text())
            (package / 'docs/TROUBLESHOOTING.md').unlink()
            with self.assertRaisesRegex(ValueError, 'Broken package links'):
                docs.check(package.resolve())

    def test_rejects_missing_source_reference_and_path_escape(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary)
            (package / 'guide.md').write_text('[missing](absent.md)\n[escape](../outside.md)')
            with self.assertRaisesRegex(ValueError, 'Broken package links'):
                docs.check(package.resolve())


if __name__ == '__main__':
    unittest.main()
