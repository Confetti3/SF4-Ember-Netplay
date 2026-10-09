"""Fault tests for the real publisher; tiny packages keep tests independent of builds."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import warnings
import zipfile

from private_build_layout import PLAY_COMMAND, publish_private_build
from private_build_retention import archive_index, assert_plain, digest, file_index

class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.dist = self.root / "dist"
        self.history = self.root / "build/private-packages"
        self.counter = 0

    def tearDown(self):
        self.tmp.cleanup()

    def candidate(self):
        self.counter += 1
        label = str(self.counter)
        parent = self.root / ("build/private-package-" + label)
        folder = parent / ("sf4-ember-netplay-" + label)
        folder.mkdir(parents=True)
        files = {"Launcher.exe": "binary " + label, "PRIVATE_TEST_BUILD.txt": "private",
                 "PackageInventory.inc": 'SF4E_PACKAGE_OBSOLETE("dxwrapper.dll")\n',
                 "build-provenance.json": json.dumps({"kind":"private-test-build","label":label,"createdUtc":"2026-10-16T00:00:00Z"})}
        for name, content in files.items():
            (folder / name).write_text(content, encoding="utf-8")
        archive = folder.with_suffix(".zip")
        with zipfile.ZipFile(archive, "w") as package:
            for file in folder.iterdir():
                package.write(file, folder.name + "/" + file.name)
        archive.with_name(archive.name + ".sha256").write_text(digest(archive) + "  " + archive.name)
        return archive

    def validate(self, folder):
        self.assertFalse((folder / "dxwrapper.dll").exists())
        self.assertFalse((folder / "unowned.exe").exists())

    def publish(self, archive=None, **kwargs):
        return publish_private_build(self.dist, archive or self.candidate(), self.history,
                                     validate=kwargs.pop("validate", self.validate), **kwargs)

    def test_rotation_keeps_one_previous(self):
        self.publish()
        first = digest(self.dist / "Share with friend.zip")
        self.publish()
        self.assertEqual(first, digest(self.history / "previous.zip"))
        second = digest(self.dist / "Share with friend.zip")
        self.publish()
        self.assertEqual(second, digest(self.history / "previous.zip"))
        self.assertEqual({"PLAY.cmd","Game","Share with friend.zip"}, {p.name for p in self.dist.iterdir()})
        self.assertFalse(list(self.history.glob(".publish-*")))

    def test_individual_preservation_and_safe_extras(self):
        self.publish()
        game = self.dist / "Game"
        (game / "Launcher.exe").write_text("user modified")
        (game / "dxwrapper.dll").write_text("obsolete")
        (game / "unowned.exe").write_text("unknown program")
        (game / "script.ps1").write_text("unknown script")
        (game / "settings.ini").write_text("player settings")
        (game / "assets").mkdir()
        (game / "assets/extra.png").write_text("unknown art")
        observed = []
        def final_check(folder):
            self.validate(folder)
            observed.append((folder / "settings.ini").read_text())
        self.publish(validate=final_check)
        self.assertEqual(["player settings"], observed)
        saved = list(self.history.glob("preserved-*"))
        self.assertEqual(1, len(saved))
        self.assertEqual({"Launcher.exe","dxwrapper.dll","unowned.exe","script.ps1","assets/extra.png"},
                         {v[0] for v in file_index(saved[0]).values()})
        self.assertNotEqual("user modified", (game / "Launcher.exe").read_text())

    def test_existing_play_untouched(self):
        self.publish()
        play = self.dist / "PLAY.cmd"
        before = play.stat().st_mtime_ns
        real = Path.write_text
        def write(path, *args, **kwargs):
            if path.name == "PLAY.cmd":
                raise AssertionError("Existing PLAY must not be written")
            return real(path,*args,**kwargs)
        with patch.object(Path,"write_text",write):
            self.publish()
        self.assertEqual(before, play.stat().st_mtime_ns)

    def test_new_play_staging_failure_preserves_no_publication(self):
        incoming = self.candidate()
        real = Path.write_text
        def write(path, *args, **kwargs):
            if path.name == "PLAY.cmd":
                raise OSError("injected staging failure")
            return real(path,*args,**kwargs)
        with patch.object(Path,"write_text",write), self.assertRaises(OSError):
            self.publish(incoming)
        self.assertFalse((self.dist / "Game").exists())
        self.assertTrue(incoming.exists())

    def test_all_rename_failures_roll_back(self):
        # Exercise first publication (including new PLAY) and replacement with
        # an existing previous ZIP and individually preserved player edits.
        for replacements, count in ((False,4),(True,9)):
            for failure in range(1,count+1):
                with self.subTest(replacements=replacements, failure=failure):
                    if self.dist.exists(): shutil.rmtree(self.dist)
                    if self.history.exists(): shutil.rmtree(self.history)
                    if replacements:
                        self.publish(); self.publish()
                        (self.dist / "Game/Launcher.exe").write_text("changed")
                    incoming = self.candidate()
                    before_dist = file_index(self.dist) if self.dist.exists() else {}
                    before_history = file_index(self.history) if self.history.exists() else {}
                    real = Path.rename
                    calls = 0
                    def rename(path,target):
                        nonlocal calls
                        calls += 1
                        if calls == failure: raise OSError("injected rename failure")
                        return real(path,target)
                    with patch.object(Path,"rename",rename), self.assertRaises(OSError):
                        self.publish(incoming)
                    self.assertEqual(before_dist, file_index(self.dist))
                    self.assertEqual(before_history, file_index(self.history))
                    self.assertTrue(incoming.exists())

    def test_cleanup_failure_warns_and_next_publish_works(self):
        self.publish()
        real = shutil.rmtree
        def remove(path,*args,**kwargs):
            if Path(path).name.startswith(".publish-"):
                raise OSError("locked old file")
            return real(path,*args,**kwargs)
        with patch("private_build_layout.shutil.rmtree",remove), warnings.catch_warnings(record=True) as caught:
            self.publish()
        self.assertTrue(caught)
        self.publish()
        self.assertEqual(1,len(list(self.history.glob(".publish-*"))))
        self.assertEqual(PLAY_COMMAND,(self.dist / "PLAY.cmd").read_text())

    def test_final_preflight_failure_leaves_current_unchanged(self):
        self.publish()
        before = file_index(self.dist)
        def fail(folder): raise ValueError("preflight rejected extras")
        with self.assertRaises(ValueError): self.publish(validate=fail)
        self.assertEqual(before,file_index(self.dist))

    def test_custom_play_refused(self):
        self.publish()
        (self.dist / "PLAY.cmd").write_text("custom")
        with self.assertRaises(ValueError): self.publish()
        self.assertEqual("custom",(self.dist / "PLAY.cmd").read_text())

    def test_corrupt_archive_and_candidate_refused(self):
        incoming = self.candidate()
        incoming.write_bytes(b"broken")
        with self.assertRaises(ValueError): self.publish(incoming)
        incoming = self.candidate()
        (incoming.with_suffix("") / "Launcher.exe").write_text("different")
        with self.assertRaises(ValueError): self.publish(incoming)

    def test_archive_case_collision_refused(self):
        incoming = self.candidate()
        with zipfile.ZipFile(incoming,"a") as package:
            package.writestr(incoming.stem + "/LAUNCHER.EXE","duplicate")
        incoming.with_name(incoming.name+".sha256").write_text(digest(incoming))
        with self.assertRaises(ValueError): archive_index(incoming)

    def test_windows_junction_and_ancestor_rejected(self):
        if os.name != "nt": return  # Windows-only path mechanism; no fake passing junction assertion.
        target, junction = self.root / "outside", self.root / "junction"
        target.mkdir()
        subprocess.run(["cmd.exe","/d","/c","mklink","/J",str(junction),str(target)],
                       check=True,capture_output=True)
        try:
            with self.assertRaises(ValueError): assert_plain(junction)
            with self.assertRaises(ValueError): assert_plain(junction / "child")
            self.publish()
            game_link = self.dist / "Game/linked"
            subprocess.run(["cmd.exe","/d","/c","mklink","/J",str(game_link),str(target)],
                           check=True,capture_output=True)
            try:
                with self.assertRaises(ValueError): self.publish()
            finally: game_link.rmdir()
        finally: junction.rmdir()

if __name__ == "__main__":
    unittest.main(verbosity=2)
