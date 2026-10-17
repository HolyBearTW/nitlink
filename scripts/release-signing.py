"""Sign final HolyBear artifacts, or verify them using only the pinned public key.

No third-party Python dependencies. Signing secrets enter through the environment
and GPG stdin, never command arguments, logs, or uploaded files.
"""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
from holybear_repository import is_authorized_actions_repository

ROOT = Path(__file__).resolve().parents[1]
SECRET_NAMES = ("HOLYBEAR_GPG_PRIVATE_KEY", "HOLYBEAR_GPG_PASSPHRASE", "HOLYBEAR_GPG_FINGERPRINT")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def identity(tag):
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    require(tag == "v" + version, "Tag does not match the source VERSION.")
    match = re.fullmatch(r"(\d+\.\d+\.\d+(?:\.\d+)?)-holybear", version)
    require(match, "Invalid HolyBear version.")
    numeric = match[1]
    return [f"NitLink-{numeric}-HolyBear-Win64.zip",
            f"NitLink-{numeric}-HolyBear-Setup-x64.exe", "SHA256SUMS.txt"]


def pinned_key():
    data = json.loads((ROOT / "HOLYBEAR-SIGNING.json").read_text(encoding="utf-8"))
    for name in ("primary_fingerprint", "signing_subkey_fingerprint"):
        require(re.fullmatch(r"[0-9A-F]{40}", data[name]), "Invalid pinned fingerprint.")
    require(data["primary_fingerprint"] != data["signing_subkey_fingerprint"], "A signing subkey is required.")
    public = ROOT / data["public_key"]
    require(public.read_bytes().startswith(b"-----BEGIN PGP PUBLIC KEY BLOCK-----"), "Missing public release key.")
    return data, public


def gpg_tools():
    executable = shutil.which("gpg")
    require(executable, "GPG executable is unavailable.")
    tools = Path(executable).parent
    agent_control = tools / ("gpgconf.exe" if os.name == "nt" else "gpgconf")
    require(agent_control.is_file(), "Matching GPG Agent control executable is unavailable.")
    cygpath = tools / "cygpath.exe" if os.name == "nt" and (tools / "msys-2.0.dll").is_file() else None
    require(cygpath is None or cygpath.is_file(), "MSYS GPG path converter is unavailable.")
    return executable, str(agent_control), cygpath


def gpg_path(path):
    # Git's MSYS GPG treats drive-letter paths as relative, even with forward
    # slashes. Native GnuPG keeps native paths; only MSYS uses cygpath.
    _, _, cygpath = gpg_tools()
    if cygpath is None:
        return str(path)
    env = {key: value for key, value in os.environ.items() if key not in SECRET_NAMES}
    result = subprocess.run([str(cygpath), "-u", str(path)], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=env, timeout=10, check=False)
    require(result.returncode == 0, "MSYS GPG path conversion failed.")
    value = result.stdout.decode("utf-8").strip()
    require(value.startswith("/") and "\n" not in value, "Invalid MSYS GPG path.")
    return value


def gpg_failure(stage, result):
    # Never print raw stderr: it may contain key UIDs or secret input diagnostics.
    # Emit only allowlisted machine status names and numeric error codes.
    diagnostic = result.stderr.decode("utf-8", errors="replace")
    allowed = {"BAD_PASSPHRASE", "MISSING_PASSPHRASE", "NO_SECKEY", "NO_PUBKEY",
               "NODATA", "IMPORT_PROBLEM", "INV_SGNR", "KEYEXPIRED", "KEYREVOKED",
               "FAILURE", "ERROR", "BADSIG", "ERRSIG"}
    statuses = []
    for line in diagnostic.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] == "[GNUPG:]" and fields[1] in allowed:
            code = fields[-1] if fields[1] in ("ERROR", "FAILURE") and fields[-1].isdigit() else None
            statuses.append(fields[1] + (f"(code={code})" if code else ""))
    text = diagnostic.lower()
    reason = "unclassified GPG error"
    if "no writable keyring" in text or ("keyblock resource" in text and "no such file" in text):
        reason = "keyring path unavailable"
    elif "bad passphrase" in text or "BAD_PASSPHRASE" in statuses:
        reason = "subkey unlock rejected: bad passphrase"
    elif "no pinentry" in text:
        reason = "pinentry unavailable"
    elif "can't connect to the agent" in text or "no gpg-agent running" in text:
        reason = "GPG Agent unavailable"
    return f"{stage} failed: {reason}; exit={result.returncode}; status={','.join(statuses) or 'none'}. Publication blocked."


def gpg(home, arguments, data=None, stage="GPG operation"):
    # Capture diagnostic output: a failure never dumps private-key input or secrets.
    env = {key: value for key, value in os.environ.items() if key not in SECRET_NAMES}
    env["LC_ALL"] = "C"
    executable, _, _ = gpg_tools()
    command = [executable, "--batch", "--no-tty", "--status-fd", "2", "--homedir", gpg_path(home),
               *(gpg_path(value) if isinstance(value, Path) else value for value in arguments)]
    try:
        result = subprocess.run(command, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                env=env, timeout=90, check=False)
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"{stage} timed out. Publication blocked.") from None
    require(result.returncode == 0, gpg_failure(stage, result))
    return result.stdout.decode("utf-8", errors="replace")


@contextlib.contextmanager
def keyring():
    # Unique per invocation, outside the checkout/artifact paths. Never share a
    # developer keyring with verification or leave an imported private key behind.
    with tempfile.TemporaryDirectory(prefix="holybear-gpg-") as folder:
        home = Path(folder)
        home.chmod(0o700)
        try:
            yield home
        finally:
            _, agent_control, _ = gpg_tools()
            subprocess.run([agent_control, "--homedir", gpg_path(home), "--kill", "all"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=30, check=False)


def validate_key(home, config, secret=False):
    stage = "Secret subkey checks" if secret else "Public key checks"
    listing = gpg(home, ["--with-colons", "--fixed-list-mode", "--list-secret-keys" if secret else "--list-keys"],
                  stage=stage)
    fingerprints, usable, pending = [], [], None
    for line in listing.splitlines():
        fields = line.split(":")
        if fields[0] in ("pub", "sub", "sec", "ssb"):
            pending = fields
        elif fields[0] == "fpr" and pending:
            fingerprint = fields[9]
            fingerprints.append(fingerprint)
            if fingerprint in (config["primary_fingerprint"], config["signing_subkey_fingerprint"]):
                require(pending[1] not in ("r", "e", "d", "i") and
                        (not pending[6] or int(pending[6]) > time.time()), f"{stage}: pinned key is unavailable, expired or revoked.")
                if fingerprint == config["signing_subkey_fingerprint"]:
                    require(pending[0] in ("sub", "ssb") and "s" in pending[11], f"{stage}: pinned subkey cannot sign.")
            if secret and len(pending) > 14 and pending[14] == "+":
                usable.append(fingerprint)
            pending = None
    require(config["primary_fingerprint"] in fingerprints and config["signing_subkey_fingerprint"] in fingerprints,
            f"{stage}: fingerprints do not match the pinned public key and signing subkey.")
    if secret:
        require(usable == [config["signing_subkey_fingerprint"]],
                f"{stage}: export must contain only the selected secret subkey, with a dummy primary key.")


def verify_hashes(assets, names):
    expected = [names[0], names[1], "RELEASE-NOTES.md"]
    records = (assets / "SHA256SUMS.txt").read_text(encoding="ascii").splitlines()
    require(len(records) == len(expected), "Checksums must cover ZIP, Setup and release notes.")
    seen = set()
    for record in records:
        match = re.fullmatch(r"([a-f0-9]{64})  (.+)", record)
        require(match and match[2] in expected and match[2] not in seen, "Invalid checksum record or path.")
        path = assets / match[2]
        require(path.is_file() and not path.is_symlink(), "Missing or unsafe release payload.")
        with path.open("rb") as stream:
            require(hashlib.file_digest(stream, "sha256").hexdigest() == match[1], "Release SHA-256 mismatch.")
        seen.add(match[2])


def verify(home, assets, names, config):
    for name in names:
        file, signature = assets / name, assets / (name + ".asc")
        require(file.is_file() and signature.is_file() and not file.is_symlink() and not signature.is_symlink(),
                "Missing detached signature or payload.")
        require(signature.read_bytes().startswith(b"-----BEGIN PGP SIGNATURE-----"), "Signature is not ASCII-armored.")
        status = gpg(home, ["--status-fd", "1", "--verify", signature, file],
                     stage=f"Signature verification: {name}")
        valid = [line.split() for line in status.splitlines() if line.startswith("[GNUPG:] VALIDSIG ")]
        require(len(valid) == 1 and valid[0][2] == config["signing_subkey_fingerprint"] and
                valid[0][-1] == config["primary_fingerprint"] and valid[0][9] in ("8", "9", "10"),
                f"Signature verification: {name}: pinned fingerprint or SHA-2 digest mismatch.")
        print(f"Verified detached signature: {name}")
    verify_hashes(assets, names)
    print("Final release SHA-256 checks passed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("sign", "verify"))
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--validation-only", action="store_true", help="Explicit trusted signing check; publication is forbidden")
    args = parser.parse_args()
    names = identity(args.tag)
    config, public = pinned_key()
    assets = args.assets.resolve(strict=True)
    verify_hashes(assets, names)
    private = passphrase = None
    if args.mode == "sign":
        require(is_authorized_actions_repository(os.environ) and
                os.environ.get("HOLYBEAR_SIGNING_ENVIRONMENT") == "release",
                "Signing requires the protected HolyBear release Environment.")
        event, ref = os.environ.get("GITHUB_EVENT_NAME"), os.environ.get("GITHUB_REF", "")
        if args.validation_only:
            require(event == "workflow_dispatch" and ref == "refs/heads/holybear/integrated",
                    "Signing validation requires Workflow Dispatch on holybear/integrated; tags are forbidden.")
        else:
            require(event == "push" and ref == "refs/tags/" + args.tag,
                    "Release signing is restricted to the matching formal tag push.")
        private, passphrase, fingerprint = (os.environ.pop(name, "") for name in SECRET_NAMES)
        require(private and passphrase and fingerprint, "All three HolyBear GPG Secrets are required; unsigned releases are forbidden.")
        require(fingerprint == config["signing_subkey_fingerprint"], "Configured signing fingerprint differs from the reviewed pinned subkey.")
        require(private.startswith("-----BEGIN PGP PRIVATE KEY BLOCK-----") and "\n" not in passphrase and "\r" not in passphrase,
                "Invalid signing secret format.")
        require(not any((assets / (name + ".asc")).exists() for name in names), "Existing signatures must never be overwritten.")
    _, _, cygpath = gpg_tools()
    print("GPG backend: " + ("MSYS with absolute POSIX paths" if cygpath else "native paths"))
    with keyring() as home:
        gpg(home, ["--import"], public.read_bytes(), stage="Public key import")
        validate_key(home, config)
        if args.mode == "sign":
            gpg(home, ["--import"], private.encode("utf-8"), stage="Secret subkey import")
            private = None
            validate_key(home, config, secret=True)
            for name in names:
                gpg(home, ["--pinentry-mode", "loopback", "--passphrase-fd", "0",
                           "--local-user", config["signing_subkey_fingerprint"] + "!", "--digest-algo", "SHA256",
                           "--armor", "--detach-sign", "--output", assets / (name + ".asc"), assets / name],
                    (passphrase + "\n").encode("utf-8"), stage=f"Subkey unlock / detached signing: {name}")
            passphrase = None
    # Verification imports only public material into a second, empty keyring.
    with keyring() as home:
        gpg(home, ["--import"], public.read_bytes(), stage="Verification public key import")
        validate_key(home, config)
        verify(home, assets, names, config)
    if args.mode == "sign":
        report = ["Verified actual GPG signing and public-key-only verification.",
                  f"validation_only={str(args.validation_only).lower()}",
                  "environment=release", f"source_ref={os.environ['GITHUB_REF']}",
                  "signing_subkey=" + config["signing_subkey_fingerprint"],
                  "primary_key=" + config["primary_fingerprint"]]
        for name in [names[0], names[0] + ".asc", names[1], names[1] + ".asc", names[2], names[2] + ".asc", "RELEASE-NOTES.md"]:
            with (assets / name).open("rb") as stream:
                report.append(hashlib.file_digest(stream, "sha256").hexdigest() + "  " + name)
        (assets / "SIGNING-VERIFICATION.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
        print("Signing validation complete; publication is a separate, strictly gated step.")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        # Report only our fixed errors; subprocess exceptions can contain arguments.
        print(f"Release signing/verification rejected: {error if isinstance(error, RuntimeError) else type(error).__name__}")
        raise SystemExit(1)
