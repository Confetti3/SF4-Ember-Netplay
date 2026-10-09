from pathlib import Path
import json
import tempfile
import unittest
import zipfile
from unittest.mock import patch

from private_build_retention import digest, prune_private_builds
from private_build_layout import publish_private_build, PLAY_COMMAND


class RetentionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='retention-test-', dir=Path.cwd())
        self.addCleanup(self.temp.cleanup)
        self.dist = Path(self.temp.name) / 'dist'
        self.dist.mkdir()

    def package(self, day, kind='private-test-build'):
        label = f'test-{day}'
        folder = self.dist / ('sf4-ember-netplay-' + label)
        folder.mkdir()
        (folder / 'PRIVATE_TEST_BUILD.txt').write_text('private')
        (folder / 'build-provenance.json').write_text(json.dumps(dict(
            kind=kind, label=label, createdUtc=f'2026-10-{day:02}T00:00:00Z')))
        (folder / 'Launcher.exe').write_bytes(b'fixture')
        archive = folder.with_suffix('.zip')
        with zipfile.ZipFile(archive, 'w') as output:
            for file in folder.iterdir():
                output.write(file, folder.name + '/' + file.name)
        archive.with_name(archive.name + '.sha256').write_text(digest(archive) + '  ' + archive.name)
        return archive

    def test_keeps_current_folder_and_two_archives(self):
        old, previous, current = [self.package(day) for day in (1, 2, 3)]
        prune_private_builds(self.dist, current)
        self.assertFalse(old.exists())
        self.assertFalse(old.with_name(old.name + '.sha256').exists())
        self.assertFalse(old.with_suffix('').exists())
        self.assertTrue(previous.exists())
        self.assertFalse(previous.with_suffix('').exists())
        self.assertTrue(current.exists())
        self.assertTrue(current.with_suffix('').is_dir())

    def test_preserves_added_settings_logs_and_modified_files(self):
        old, previous, current = [self.package(day) for day in (1, 2, 3)]
        (old.with_suffix('') / 'settings.json').write_text('my settings')
        (previous.with_suffix('') / 'Launcher.exe').write_bytes(b'changed')
        removed, preserved = prune_private_builds(self.dist, current)
        self.assertFalse(removed)
        self.assertEqual(len(preserved), 2)
        self.assertTrue(old.exists())
        self.assertEqual((old.with_suffix('') / 'settings.json').read_text(), 'my settings')

    def test_invalid_current_never_deletes_old_output(self):
        old, current = [self.package(day) for day in (1, 2)]
        current.write_bytes(b'broken')
        with self.assertRaises(ValueError):
            prune_private_builds(self.dist, current)
        self.assertTrue(old.exists())
        self.assertTrue(old.with_suffix('').exists())

    def test_leaves_public_and_corrupt_archives_untouched(self):
        public = self.package(1, kind='release')
        corrupt = self.package(2)
        corrupt.write_bytes(b'broken')
        current = self.package(3)
        prune_private_builds(self.dist, current)
        self.assertTrue(public.exists())
        self.assertTrue(public.with_suffix('').exists())
        self.assertEqual(corrupt.read_bytes(), b'broken')

    def test_rejects_current_outside_dist(self):
        current = self.package(1)
        other = self.dist / 'other'
        other.mkdir()
        with self.assertRaises(ValueError):
            prune_private_builds(other, current)
        self.assertTrue(current.exists())

    def test_second_run_does_not_remove_retained_packages(self):
        previous, current = [self.package(day) for day in (1, 2)]
        prune_private_builds(self.dist, current)
        removed, preserved = prune_private_builds(self.dist, current)
        self.assertEqual((removed, preserved), ([], []))
        self.assertTrue(previous.exists())
        self.assertTrue(current.exists())

    def publish(self, archive):
        return publish_private_build(self.dist, archive, self.dist.parent / 'build/private-packages')

    def test_clear_layout_and_single_backup_after_three_builds(self):
        first = self.package(1)
        first_hash = digest(first)
        self.publish(first)
        second = self.package(2)
        second_hash = digest(second)
        self.publish(second)
        self.assertEqual(digest(self.dist.parent / 'build/private-packages/previous.zip'), first_hash)
        self.publish(self.package(3))
        self.assertEqual({p.name for p in self.dist.iterdir()}, {'Game', 'Share with friend.zip', 'PLAY.cmd'})
        self.assertEqual((self.dist / 'PLAY.cmd').read_text(), PLAY_COMMAND)
        history = self.dist.parent / 'build/private-packages'
        self.assertEqual(digest(history / 'previous.zip'), second_hash)
        self.assertEqual({p.name for p in history.iterdir()}, {'previous.zip', 'previous.zip.sha256', 'current.sha256'})

    def test_player_files_follow_game_and_modified_original_is_preserved(self):
        self.publish(self.package(1))
        (self.dist / 'Game/settings.json').write_text('player settings')
        (self.dist / 'Game/Launcher.exe').write_bytes(b'customized')
        game, share, saved = self.publish(self.package(2))
        self.assertEqual((game / 'settings.json').read_text(), 'player settings')
        self.assertEqual((game / 'Launcher.exe').read_bytes(), b'fixture')
        self.assertEqual((saved / 'Launcher.exe').read_bytes(), b'customized')

    def test_failed_publication_restores_current_and_rollback(self):
        self.publish(self.package(1))
        self.publish(self.package(2))
        share = self.dist / 'Share with friend.zip'
        previous = self.dist.parent / 'build/private-packages/previous.zip'
        before = digest(share), digest(previous)
        incoming = self.package(3)
        original_rename = Path.rename
        def fail_archive_move(source, target):
            if source == incoming and target == share:
                raise PermissionError('simulated locked archive')
            return original_rename(source, target)
        with patch.object(Path, 'rename', fail_archive_move):
            with self.assertRaises(PermissionError):
                self.publish(incoming)
        self.assertEqual((digest(share), digest(previous)), before)
        self.assertTrue((self.dist / 'Game/Launcher.exe').exists())
        self.assertTrue(incoming.with_suffix('').exists())

    def test_added_player_files_do_not_accumulate_full_game_backups(self):
        self.publish(self.package(1))
        (self.dist / 'Game/session.log').write_text('player log')
        game, share, saved = self.publish(self.package(2))
        self.assertIsNone(saved)
        self.assertEqual((game / 'session.log').read_text(), 'player log')
        self.assertFalse(list((self.dist.parent / 'build/private-packages').glob('preserved-*')))

    def test_bad_incoming_never_replaces_current(self):
        self.publish(self.package(1))
        share = self.dist / 'Share with friend.zip'
        before = digest(share)
        incoming = self.package(2)
        incoming.write_bytes(b'broken')
        with self.assertRaises(ValueError):
            self.publish(incoming)
        self.assertEqual(digest(share), before)

    def test_history_cannot_escape_build_directory(self):
        incoming = self.package(1)
        with self.assertRaises(ValueError):
            publish_private_build(self.dist, incoming, self.dist.parent / 'elsewhere')
        self.assertTrue(incoming.exists())

    def test_candidate_can_publish_from_temporary_build_staging(self):
        incoming = self.package(1)
        build = self.dist.parent / 'build'
        build.mkdir()
        with tempfile.TemporaryDirectory(prefix='private-package-', dir=build) as temp:
            staging = Path(temp)
            for source in (incoming.with_suffix(''), incoming, incoming.with_name(incoming.name + '.sha256')):
                source.rename(staging / source.name)
            self.publish(staging / incoming.name)
        self.assertEqual({p.name for p in self.dist.iterdir()}, {'Game', 'Share with friend.zip', 'PLAY.cmd'})
        self.assertFalse(staging.exists())


if __name__ == '__main__':
    unittest.main()
