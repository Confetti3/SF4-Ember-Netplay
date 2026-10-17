import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("package_docs", Path(__file__).with_name("package-docs.py"))
docs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(docs)


class PackageDocsTest(unittest.TestCase):
    def test_commonmark_destinations_only(self):
        tick = chr(96)
        text = (
            '[![Picture](image(a).png "inner")](guide(b).md#part "outer")\n\n'
            '[ref][ID]\n\n[ID]: <folder/doc(c).md> "title"\n\n'
            '> [quote][Q]\n>\n> [Q]: quoted(d).md\n\n'
            '- [list](list(e).md)\n\n'
            + tick + '[example](absent.md)' + tick + '\n\n'
            + tick * 3 + 'md\n[example](absent.md)\n' + tick * 3 + '\n\n'
            '    [indented](absent.md)\n\n'
            '[escaped](file\\(f\\).md)\n'
        )
        seen = []
        def callback(target):
            seen.append(target)
            return "moved/" + target
        result = docs.transform(text, callback)
        self.assertEqual(seen, ["image(a).png", "guide(b).md#part", "folder/doc(c).md", "quoted(d).md", "list(e).md", "file(f).md"])
        self.assertEqual(result.count("[example](absent.md)"), 2)
        self.assertIn("    [indented](absent.md)", result)
        self.assertIn('[ID]: <moved/folder/doc(c).md> "title"', result)
        self.assertIn('> [Q]: moved/quoted(d).md', result)
        self.assertIn('[![Picture](moved/image(a).png "inner")](moved/guide(b).md#part "outer")', result)

    def test_staged_content_and_explicit_renamed_guide_alias(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            repo, stage, package = root / "repo", root / "stage", root / "package"
            (repo / "docs/guides").mkdir(parents=True)
            (repo / "src").mkdir()
            (stage / "docs/guides").mkdir(parents=True)
            (package / "docs").mkdir(parents=True)
            original = repo / "docs/guides/USER.md"
            help_file = repo / "docs/guides/help(test).md"
            original.write_text("OLD REPOSITORY CONTENT",encoding="utf-8")
            help_file.write_text("[Start](USER.md)",encoding="utf-8")
            (repo / "src/source.py").write_text("pass",encoding="utf-8")
            selected = stage / "docs/guides/USER.md"
            selected.write_text("STAGED CONTENT\n[Help](help(test).md#logs)\n[Source](../../../repo/src/source.py)\n",encoding="utf-8")
            shutil.copyfile(selected,package / "START_HERE.md")
            shutil.copyfile(help_file,package / "docs/help(test).md")
            records = [
                dict(source=str(selected),aliases=[str(original)],destination="START_HERE.md",preferred=True),
                dict(source=str(help_file),aliases=[str(stage / "docs/guides/help(test).md")],destination="docs/help(test).md"),
            ]
            self.assertEqual(docs.relocate(repo,package,"a"*40,records),2)
            text = (package / "START_HERE.md").read_text(encoding="utf-8")
            self.assertIn("STAGED CONTENT",text)
            self.assertNotIn("OLD REPOSITORY",text)
            self.assertIn("(docs/help%28test%29.md#logs)",text)
            self.assertIn("/blob/"+"a"*40+"/src/source.py",text)
            self.assertIn("(../START_HERE.md)",(package/"docs/help(test).md").read_text(encoding="utf-8"))
            (package/"docs/help(test).md").unlink()
            with self.assertRaisesRegex(ValueError,"Broken package links"):
                docs.check(package)

    def test_quoted_and_nested_missing_links_are_detected_but_code_is_ignored(self):
        with tempfile.TemporaryDirectory() as temporary:
            package=Path(temporary)
            document=package/"guide.md"
            document.write_text(chr(96)+"[sample](missing.md)"+chr(96),encoding="utf-8")
            self.assertEqual(docs.check(package),1)
            document.write_text('[![nested](missing(a).png)](https://example.com)\n',encoding="utf-8")
            with self.assertRaisesRegex(ValueError,"missing\\(a\\).png"): docs.check(package)
            (package/"image.png").write_bytes(b"fixture")
            document.write_text('[![nested](image.png)](missing-outer.md)',encoding="utf-8")
            with self.assertRaisesRegex(ValueError,"missing-outer.md"): docs.check(package)
            document.write_text('[r][id]\n\n[id]: <missing(b).md>\n',encoding="utf-8")
            with self.assertRaisesRegex(ValueError,"missing\\(b\\).md"): docs.check(package)


    def copy_inventory(self, repo, stage, package, quick="", full_art=False, staged_binaries=False):
        import json
        import subprocess
        runner = package.parent / "copy.ps1"
        mapping = package.parent / "mapping.json"
        runner.write_text(r'''
param($Repo,$Stage,$Package,$Helper,$Mapping,$Quick,[switch]$SelectionArt,[switch]$StagedBinariesOnly)
$ErrorActionPreference='Stop'
. $Helper
$entries = @(foreach($line in Get-Content -LiteralPath (Join-Path $Repo 'src/common/PackageInventory.inc')){
    if($line -match '^SF4E_PACKAGE_(REQUIRED|OPTIONAL)\("([^"]+\.(?:md|exe|dll))"\)'){
        if(!$StagedBinariesOnly -and $Matches[2] -notlike '*.md'){continue}
        [pscustomobject]@{Required=$Matches[1]-eq'REQUIRED';Path=$Matches[2].Replace('\\','\')}
    }
})
$records=@(Copy-EmberPackageContent -Repo $Repo -BuildDir (Join-Path $Package 'absent-build') -InstallDir $Stage -Destination $Package -Entries $entries -Generated @('START_HERE.md') -QuickStartPath $Quick -SkipSelectionArt:(!$SelectionArt) -StagedBinariesOnly:$StagedBinariesOnly)
ConvertTo-Json -InputObject $records -Depth 5 | Set-Content -LiteralPath $Mapping -Encoding utf8
''',encoding="utf-8")
        helper = Path(__file__).with_name("PackageContent.ps1")
        pwsh = shutil.which("pwsh")
        self.assertIsNotNone(pwsh, "PowerShell 7 is required for the real package-copy fixture")
        subprocess.run([pwsh,"-NoProfile","-File",str(runner),str(repo),str(stage),str(package),str(helper),str(mapping),str(quick)] + (["-SelectionArt"] if full_art else []) + (["-StagedBinariesOnly"] if staged_binaries else []),check=True)
        return json.loads(mapping.read_text(encoding="utf-8-sig"))

    def test_real_inventory_copy_and_relocation(self):
        repo = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary) / "package"
            package.mkdir()
            records = self.copy_inventory(repo, package.parent / "absent-stage", package, full_art=True)
            self.assertGreater(len(records), 10)
            self.assertEqual(docs.relocate(repo,package,"a"*40,records),sum(r["destination"].lower().endswith(".md") for r in records))
            self.assertIn("(docs/TROUBLESHOOTING.md)",(package/"START_HERE.md").read_text(encoding="utf-8"))
            self.assertIn("(SAVING_LOGS.md)",(package/"docs/TROUBLESHOOTING.md").read_text(encoding="utf-8"))

    def test_real_copy_preserves_staged_documents_and_custom_start(self):
        repo = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage, package = root/"stage", root/"package"
            (stage/"docs").mkdir(parents=True)
            package.mkdir()
            selected = stage/"docs/USER_NETPLAY.md"
            selected.write_text("STAGED guide\n[Help](TROUBLESHOOTING.md)",encoding="utf-8")
            (stage/"docs/TROUBLESHOOTING.md").write_text("STAGED support",encoding="utf-8")
            quick = root/"custom-start.md"
            quick.write_text("CUSTOM start\n[Help](stage/docs/TROUBLESHOOTING.md)",encoding="utf-8")
            records = self.copy_inventory(repo,stage,package,quick)
            chosen = next(r for r in records if r["destination"] == "docs/USER_NETPLAY.md")
            self.assertEqual(Path(chosen["source"]),selected)
            docs.relocate(repo,package,"a"*40,records)
            self.assertEqual((package/"docs/USER_NETPLAY.md").read_text(encoding="utf-8"),"STAGED guide\n[Help](TROUBLESHOOTING.md)")
            self.assertEqual((package/"docs/TROUBLESHOOTING.md").read_text(encoding="utf-8"),"STAGED support")
            self.assertEqual((package/"START_HERE.md").read_text(encoding="utf-8"),"CUSTOM start\n[Help](docs/TROUBLESHOOTING.md)")

    def test_private_copy_rejects_unstaged_binary_fallback(self):
        import subprocess
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            repo, stage, package = root/"repo", root/"stage", root/"package"
            (repo/"src/common").mkdir(parents=True)
            (repo/"docs/guides").mkdir(parents=True)
            stage.mkdir()
            package.mkdir()
            (repo/"src/common/PackageInventory.inc").write_text('SF4E_PACKAGE_REQUIRED("Launcher.exe")',encoding="utf-8")
            (repo/"docs/guides/USER_NETPLAY.md").write_text("Guide",encoding="utf-8")
            (repo/"Launcher.exe").write_bytes(b"unverified repository binary")
            with self.assertRaises(subprocess.CalledProcessError):
                self.copy_inventory(repo,stage,package,staged_binaries=True)
            self.assertFalse((package/"Launcher.exe").exists())
            (stage/"Launcher.exe").write_bytes(b"verified staged binary")
            records = self.copy_inventory(repo,stage,package,staged_binaries=True)
            self.assertEqual((package/"Launcher.exe").read_bytes(),b"verified staged binary")
            chosen = next(r for r in records if r["destination"] == "Launcher.exe")
            self.assertEqual(Path(chosen["source"]),stage/"Launcher.exe")

    def test_mapping_and_path_errors_are_not_silently_ignored(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            source=root/"source.md"
            source.write_text("plain",encoding="utf-8")
            package=root/"package"
            package.mkdir()
            (package/"guide.md").write_text("[escape](../source.md)",encoding="utf-8")
            with self.assertRaisesRegex(ValueError,"Broken package links"):
                docs.check(package)
            record=dict(source=str(source),destination="guide.md")
            with self.assertRaisesRegex(ValueError,"Duplicate"):
                docs.relocate(root,package,"a"*40,[record,record])
            with self.assertRaisesRegex(ValueError,"Every packaged Markdown"):
                docs.relocate(root,package,"a"*40,[])
            with self.assertRaisesRegex(ValueError,"outside-package"):
                docs.relocate(root,package,"a"*40,[dict(source=str(source),destination="../source.md")])


if __name__ == "__main__":
    unittest.main()
