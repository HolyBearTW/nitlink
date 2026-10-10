"""Public-key-only GPG preflight and secret-safe failure diagnostics."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("release_signing", Path(__file__).with_name("release-signing.py"))
signing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(signing)


class ReleaseSigningTests(unittest.TestCase):
    def test_failure_diagnostics_redact_untrusted_content(self):
        marker = "DO_NOT_PRINT_SECRET_OR_UID"
        result = subprocess.CompletedProcess([], 2, b"", (
            "gpg: " + marker + "\n[GNUPG:] USERID_HINT " + marker +
            "\n[GNUPG:] ERROR pk_sign 67108875\n[GNUPG:] FAILURE sign 67108875\n"
            "[GNUPG:] BAD_PASSPHRASE " + marker + "\n").encode())
        message = signing.gpg_failure("Subkey unlock", result)
        self.assertNotIn(marker, message)
        self.assertIn("Subkey unlock failed", message)
        self.assertIn("BAD_PASSPHRASE", message)
        self.assertIn("ERROR(code=67108875)", message)

    def test_keyring_path_failure_reports_the_stage(self):
        result = subprocess.CompletedProcess([], 2, b"",
            b"gpg: no writable keyring found\nsensitive untrusted diagnostic")
        message = signing.gpg_failure("Public key import", result)
        self.assertIn("Public key import failed: keyring path unavailable", message)
        self.assertNotIn("sensitive", message)

    def test_public_key_import_in_available_native_and_msys_backends(self):
        # This invokes real GPG with ONLY the committed public key. In particular,
        # the Git/MSYS backend reproduces the old drive-letter homedir failure.
        backends = []
        available = shutil.which("gpg")
        if available:
            backends.append(Path(available).parent)
        if os.name == "nt":
            git = shutil.which("git")
            self.assertIsNotNone(git, "Git is required on the Windows build runner")
            git_tools = Path(git).parent.parent / "usr/bin"
            self.assertTrue((git_tools / "gpg.exe").is_file(), "Git GPG is missing")
            backends.append(git_tools)
        self.assertTrue(backends, "GPG is required for release validation")
        config, public = signing.pinned_key()
        for tools in dict.fromkeys(backends):
            with self.subTest(backend=str(tools)), patch.dict(os.environ,
                    {"PATH": str(tools) + os.pathsep + os.environ["PATH"]}):
                with signing.keyring() as home:
                    signing.gpg(home, ["--import"], public.read_bytes(), stage="Preflight public key import")
                    signing.validate_key(home, config)


if __name__ == "__main__":
    unittest.main()

