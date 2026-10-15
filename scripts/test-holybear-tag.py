"""Real tag cryptography using disposable keys/repos; never touch release secrets.

Mock event contexts run only extracted source guards, not signing or publication.
"""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("tag_verifier", ROOT / "scripts/verify-holybear-tag.py")
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)
TAG = "v" + (ROOT / "VERSION").read_text(encoding="utf-8-sig").strip()


class TagSecurityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="nitlink-tag-fixtures-")
        cls.folder = Path(cls.temporary.name).resolve()
        assert cls.folder.parent == Path(tempfile.gettempdir()).resolve()
        cls.key_context = verifier.signing.keyring()
        cls.home = cls.key_context.__enter__()
        cls.gpg_executable = verifier.signing.gpg_tools()[0]
        cls.env = {key: value for key, value in os.environ.items() if key not in verifier.signing.SECRET_NAMES}
        cls.env["GNUPGHOME"] = verifier.signing.gpg_path(cls.home)
        cls.env.pop("GIT_DIR", None)
        cls.env.pop("GIT_WORK_TREE", None)
        cls.fingerprints = []
        for index in range(2):
            verifier.signing.gpg(cls.home, ["--pinentry-mode", "loopback", "--passphrase", "",
                "--quick-generate-key", f"Disposable Tag Fixture {index} <fixture{index}@example.invalid>", "ed25519", "cert", "0"])
            listing = verifier.signing.gpg(cls.home, ["--with-colons", "--list-keys"])
            primary = [line.split(":")[9] for line in listing.splitlines() if line.startswith("fpr:")][-1]
            verifier.signing.gpg(cls.home, ["--pinentry-mode", "loopback", "--passphrase", "", "--quick-add-key", primary, "ed25519", "sign", "0"])
            listing = verifier.signing.gpg(cls.home, ["--with-colons", "--list-keys", primary])
            fingerprints = [line.split(":")[9] for line in listing.splitlines() if line.startswith("fpr:")]
            cls.fingerprints.append(fingerprints)
        cls.primary, cls.signer = cls.fingerprints[0]
        cls.unapproved_primary, cls.unapproved_signer = cls.fingerprints[1]
        public = verifier.signing.gpg(cls.home, ["--armor", "--export", cls.primary, cls.unapproved_primary])
        cls.base = cls.folder / "base"
        cls.base.mkdir()
        cls.run_tool(["git", "init", "-b", "holybear/integrated", str(cls.base)])
        for name in ("VERSION", "CMakeLists.txt", "README-HOLYBEAR.txt", "src/app.rc", "src/app/holybear_version.h",
                     "src/overlay/overlay.cpp", "nitlink-menu.html", "assets/menu/menu.js",
                     "scripts/holy-bear-common.ps1", "scripts/verify-holybear-tag.py", "scripts/release-signing.py",
                     "scripts/publish-holybear.ps1", "scripts/holy-bear-release-notes.ps1"):
            destination = cls.base / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, destination)
        (cls.base / "licenses").mkdir()
        (cls.base / "licenses/HOLYBEAR-RELEASE-PUBLIC-KEY.asc").write_text(public, encoding="ascii")
        cls.policy = {"primary_fingerprint": cls.primary, "allowed_signing_fingerprints": [cls.signer],
                      "public_key": "licenses/HOLYBEAR-RELEASE-PUBLIC-KEY.asc"}
        (cls.base / verifier.POLICY_FILE).write_text(json.dumps(cls.policy), encoding="utf-8")
        cls.run_tool(["git", "-C", str(cls.base), "config", "user.name", "Disposable Fixture"])
        cls.run_tool(["git", "-C", str(cls.base), "config", "user.email", "fixture@example.invalid"])
        cls.run_tool(["git", "-C", str(cls.base), "config", "commit.gpgsign", "false"])
        cls.run_tool(["git", "-C", str(cls.base), "add", "."])
        cls.run_tool(["git", "-C", str(cls.base), "commit", "-m", "Disposable reviewed source"])
        cls.commit = cls.run_tool(["git", "-C", str(cls.base), "rev-parse", "HEAD"]).stdout.decode().strip()

    @classmethod
    def tearDownClass(cls):
        cls.key_context.__exit__(None, None, None)
        # Only the explicitly owned temporary fixture directory is removed.
        assert cls.folder.parent == Path(tempfile.gettempdir()).resolve()
        cls.temporary.cleanup()

    @classmethod
    def run_tool(cls, command, *, data=None, cwd=None, env=None, check=True):
        result = subprocess.run(command, input=data, cwd=cwd, env=env or cls.env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=45)
        if check and result.returncode:
            raise RuntimeError("Fixture operation failed: " + result.stderr.decode("utf-8", errors="replace"))
        return result

    def setUp(self):
        self.repo = self.folder / self._testMethodName
        self.run_tool(["git", "clone", "--no-hardlinks", str(self.base), str(self.repo)])
        self.run_tool(["git", "-C", str(self.repo), "config", "user.name", "Disposable Fixture"])
        self.run_tool(["git", "-C", str(self.repo), "config", "user.email", "fixture@example.invalid"])
        self.run_tool(["git", "-C", str(self.repo), "config", "commit.gpgsign", "false"])
        self.run_tool(["git", "-C", str(self.repo), "config", "tag.gpgsign", "false"])

    def git(self, *arguments, **options):
        return self.run_tool(["git", "-C", str(self.repo), *arguments], **options)

    def signed(self, signer=None, commit=None, tag=TAG):
        self.git("-c", "gpg.format=openpgp", "-c", f"gpg.program={self.gpg_executable}",
                 "-c", f"gpg.openpgp.program={self.gpg_executable}", "tag", "-s", "-u", (signer or self.signer) + "!",
                 "-m", "Disposable source signature", tag, commit or self.commit)

    def verify(self, tag=TAG, commit=None):
        return verifier.verify_tag(self.repo, tag, commit or self.commit)

    def rejected(self):
        with self.assertRaises(RuntimeError):
            self.verify()

    def workflow_source(self, step):
        lines = (ROOT / ".github/workflows/holy-bear-release.yml").read_text(encoding="utf-8").splitlines()
        start = next(i for i, line in enumerate(lines) if line == "      - name: " + step)
        start = next(i for i in range(start, len(lines)) if lines[i] == "        run: |") + 1
        end = next((i for i in range(start, len(lines)) if lines[i] and not lines[i].startswith("          ")), len(lines))
        return "\n".join(line[10:] for line in lines[start:end])

    def guard(self, source, event="push", ref="refs/tags/" + TAG, validation="false"):
        script = self.repo / "fixture-source-guard.ps1"
        marker = self.repo / "guard-passed.txt"
        marker.unlink(missing_ok=True)
        (self.repo / "mock-output.txt").write_text("", encoding="utf-8")
        script.write_text("$ErrorActionPreference='Stop'\n" + source + "\nSet-Content -LiteralPath 'guard-passed.txt' -Value 'passed'\n", encoding="utf-8")
        env = dict(self.env, GITHUB_EVENT_NAME=event, GITHUB_REF=ref, SIGNING_VALIDATION=validation,
                   RELEASE_TAG=TAG, GITHUB_OUTPUT=str(self.repo / "mock-output.txt"), GITHUB_PATH=str(self.repo / "mock-path.txt"))
        result = self.run_tool(["pwsh", "-NoProfile", "-File", str(script)], cwd=self.repo, env=env, check=False)
        return result, marker.exists()

    def test_approved_annotated_tag_and_gates(self):
        self.signed()
        result = self.verify()
        self.assertEqual(result["signer"], self.signer)
        self.assertEqual(result["primary"], self.primary)
        for step in ("Select build, signing validation or formal release", "Require a trusted source and locate GPG"):
            with self.subTest(step=step):
                result, passed = self.guard(self.workflow_source(step))
                self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
                self.assertTrue(passed)

    def test_lightweight_tag(self):
        self.git("tag", TAG)
        self.rejected()

    def test_unsigned_annotated_tag(self):
        self.git("tag", "-a", "-m", "Unsigned", TAG)
        self.rejected()

    def test_tampered_signature(self):
        self.signed()
        data = self.git("cat-file", "tag", "refs/tags/" + TAG).stdout.replace(b"Disposable source signature", b"Tampered source message")
        object_id = self.git("hash-object", "-t", "tag", "-w", "--stdin", data=data).stdout.decode().strip()
        self.git("update-ref", "refs/tags/" + TAG, object_id)
        self.rejected()

    def test_unapproved_but_cryptographically_valid_key(self):
        self.signed(self.unapproved_signer)
        self.rejected()

    def test_wrong_commit(self):
        tree = self.git("rev-parse", "HEAD^{tree}").stdout.decode().strip()
        other = self.git("commit-tree", tree, "-p", self.commit, "-m", "Other Commit").stdout.decode().strip()
        self.signed(commit=other)
        self.rejected()

    def test_wrong_tag_name(self):
        self.signed(tag="v9.9.9-holybear")
        with self.assertRaises(RuntimeError):
            self.verify(tag="v9.9.9-holybear")

    def test_commit_outside_reviewed_history(self):
        tree = self.git("rev-parse", "HEAD^{tree}").stdout.decode().strip()
        other = self.git("commit-tree", tree, "-p", self.commit, "-m", "Unreviewed Commit").stdout.decode().strip()
        self.git("checkout", "--detach", other)
        self.signed(commit=other)
        with self.assertRaises(RuntimeError):
            self.verify(commit=other)

    def test_missing_tag_object(self):
        self.rejected()

    def test_missing_reviewed_ref(self):
        self.signed()
        self.git("update-ref", "-d", verifier.REVIEWED_REF)
        self.rejected()

    def test_missing_gpg(self):
        self.signed()
        with patch.object(verifier.signing, "gpg_tools", side_effect=RuntimeError("GPG missing")):
            self.rejected()

    def test_missing_public_key_from_trusted_tree(self):
        self.signed()
        self.git("rm", "licenses/HOLYBEAR-RELEASE-PUBLIC-KEY.asc")
        tree = self.git("write-tree").stdout.decode().strip()
        reviewed = self.git("commit-tree", tree, "-p", self.commit, "-m", "Missing key").stdout.decode().strip()
        self.git("update-ref", verifier.REVIEWED_REF, reviewed)
        self.rejected()

    def test_tag_checkout_cannot_override_reviewed_trust(self):
        self.signed(self.unapproved_signer)
        policy = dict(self.policy, primary_fingerprint=self.unapproved_primary, allowed_signing_fingerprints=[self.unapproved_signer])
        (self.repo / verifier.POLICY_FILE).write_text(json.dumps(policy), encoding="utf-8")
        self.rejected()

    def test_global_keyring_and_git_gpg_config_do_not_authorize_signer(self):
        self.signed(self.unapproved_signer)
        self.git("config", "gpg.program", "nonexistent-global-gpg")
        with patch.dict(os.environ, self.env):
            self.rejected()

    def test_bad_signature_statuses_are_fatal(self):
        valid = f"[GNUPG:] VALIDSIG {self.signer} 2026-10-12 1791800000 0 4 0 22 8 00 {self.primary}\n"
        cases = ["[GNUPG:] GOODSIG 0123456789abcdef Fixture\n", valid.replace(" 8 00 ", " 2 00 "),
                 valid.replace(" 0 4 0 ", " 1 4 0 "), valid + valid]
        cases += [valid + "[GNUPG:] " + status + " Fixture\n" for status in verifier.BAD_STATUS]
        for raw in cases:
            with self.subTest(raw=raw):
                with self.assertRaises(RuntimeError):
                    verifier.signature_status(subprocess.CompletedProcess([], 0, b"", raw.encode()), self.policy)
        with self.assertRaises(RuntimeError):
            verifier.signature_status(subprocess.CompletedProcess([], 1, b"", valid.encode()), self.policy)

    def test_invalid_expired_revoked_disabled_key_metadata(self):
        def listing(validity="-", capabilities="s", expiry="", parent=None):
            return f"pub:-:256:22:0:1::::::c:\nfpr:::::::::{parent or self.primary}:\nsub:{validity}:256:22:0:1:{expiry}:::::{capabilities}:\nfpr:::::::::{self.signer}:\n"
        verifier.usable_keys(listing(), self.primary, self.signer)
        for value in [listing(validity=x) for x in ("r", "e", "d", "i", "n")] + [listing(capabilities="sD"), listing(capabilities="e"), listing(expiry="1"), listing(parent=self.unapproved_primary)]:
            with self.assertRaises(RuntimeError):
                verifier.usable_keys(value, self.primary, self.signer)

    def test_preview_and_both_manual_modes_without_tag(self):
        source = self.workflow_source("Select build, signing validation or formal release")
        for event, validation in (("push", "false"), ("workflow_dispatch", "false"), ("workflow_dispatch", "true")):
            with self.subTest(event=event, validation=validation):
                result, passed = self.guard(source, event, "refs/heads/holybear/integrated", validation)
                self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
                self.assertTrue(passed)
                output = (self.repo / "mock-output.txt").read_text(encoding="utf-8-sig")
                self.assertIn("release=false", output)
                self.assertIn("validate=" + ("true" if event == "workflow_dispatch" and validation == "true" else "false"), output)
        result, passed = self.guard(self.workflow_source("Require a trusted source and locate GPG"), "workflow_dispatch", "refs/heads/holybear/integrated", "true")
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        self.assertTrue(passed)

    def test_formal_gates_fail_closed_before_side_effects(self):
        self.git("tag", "-a", "-m", "Unsigned", TAG)
        for step in ("Select build, signing validation or formal release", "Require a trusted source and locate GPG"):
            result, passed = self.guard(self.workflow_source(step))
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(passed)
        publisher = (ROOT / "scripts/publish-holybear.ps1").read_text(encoding="utf-8")
        package = (ROOT / "scripts/package-holybear.ps1").read_text(encoding="utf-8")
        self.assertLess(publisher.index("Assert-HolyBearSignedTag $Tag $commit"), publisher.index("gh release create"))
        self.assertIn("if ($RequireTag) { Assert-HolyBearSignedTag $Tag $commit }", package)
        source = ". ./scripts/holy-bear-common.ps1\nAssert-HolyBearSignedTag '" + TAG + "' (Get-HolyBearCommit)"
        result, passed = self.guard(source)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(passed)

    def test_publisher_rejects_unsigned_tag_before_any_github_call(self):
        self.git("tag", "-a", "-m", "Unsigned", TAG)
        source = ("function gh { throw 'A GitHub call must never be reached.' }\n"
                  "& ./scripts/publish-holybear.ps1 -Tag '" + TAG + "' -Assets '.'")
        env = dict(self.env, GITHUB_ACTIONS="true", GITHUB_REPOSITORY="HolyBearTW/nitlink")
        with patch.dict(self.env, env):
            result, passed = self.guard(source)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(passed)
        self.assertIn("Formal source tag GPG verification failed", result.stderr.decode(errors="replace"))
        self.assertNotIn("A GitHub call must never be reached", result.stderr.decode(errors="replace"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
