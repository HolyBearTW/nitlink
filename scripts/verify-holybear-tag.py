"""Verify a formal source tag against the reviewed integration branch's public key.

No private key or global GPG keyring is used. Artifact signing policy is separate.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import time

spec = importlib.util.spec_from_file_location("artifact_signing", Path(__file__).with_name("release-signing.py"))
signing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(signing)
REVIEWED_REF = "refs/remotes/origin/holybear/integrated"
POLICY_FILE = "HOLYBEAR-TAG-SIGNING.json"
BAD_STATUS = {"BADSIG", "ERRSIG", "EXPSIG", "EXPKEYSIG", "REVKEYSIG", "KEYEXPIRED",
              "KEYREVOKED", "SIGEXPIRED", "NO_PUBKEY", "NODATA", "FAILURE", "ERROR"}
require = signing.require


def git(repository, *arguments, env=None, data=None, check=True):
    clean_env = {key: value for key, value in (env or os.environ).items() if key not in signing.SECRET_NAMES}
    clean_env["GIT_NO_REPLACE_OBJECTS"] = "1"
    try:
        result = subprocess.run(["git", "-C", str(repository), *arguments], input=data,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                env=clean_env, timeout=30, check=False)
    except (OSError, subprocess.TimeoutExpired):
        raise RuntimeError("Required Git operation unavailable or timed out; publication blocked.") from None
    require(not check or result.returncode == 0, "Required Git object/ref or operation missing; publication blocked.")
    return result


def trusted_blob(repository, revision, path):
    relative = PurePosixPath(path)
    require(not relative.is_absolute() and ".." not in relative.parts and "\\" not in path and ":" not in path,
            "Invalid trusted public-key path.")
    entry = git(repository, "ls-tree", revision, "--", path).stdout.decode("utf-8")
    require(entry.startswith("100644 blob ") or entry.startswith("100755 blob "), "Trusted policy/key must be a regular tracked file.")
    return git(repository, "show", f"{revision}:{path}").stdout


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "Duplicate tag trust policy field.")
        result[key] = value
    return result


def trust_policy(repository, revision):
    data = json.loads(trusted_blob(repository, revision, POLICY_FILE).decode("utf-8"), object_pairs_hook=unique_object)
    require(set(data) == {"primary_fingerprint", "allowed_signing_fingerprints", "public_key"}, "Invalid tag trust policy fields.")
    primary, allowed = data["primary_fingerprint"], data["allowed_signing_fingerprints"]
    require(isinstance(primary, str) and re.fullmatch(r"[0-9A-F]{40}", primary), "Invalid approved primary fingerprint.")
    require(isinstance(allowed, list) and allowed and all(isinstance(value, str) and re.fullmatch(r"[0-9A-F]{40}", value) for value in allowed)
            and len(set(allowed)) == len(allowed), "Invalid approved tag signer list.")
    require(isinstance(data["public_key"], str), "Invalid trusted public-key path type.")
    public = trusted_blob(repository, revision, data["public_key"])
    require(public.startswith(b"-----BEGIN PGP PUBLIC KEY BLOCK-----"), "Trusted OpenPGP public key missing.")
    return data, public


def usable_keys(listing, primary, signer):
    keys, pending, parent = {}, None, None
    for line in listing.splitlines():
        fields = line.split(":")
        if fields[0] in ("pub", "sub"):
            require(len(fields) > 11, "Incomplete GPG key metadata.")
            pending = fields
        elif fields[0] == "fpr" and pending:
            fingerprint = fields[9]
            if pending[0] == "pub":
                parent = fingerprint
            keys[fingerprint] = (pending, parent)
            pending = None
    require(primary in keys and keys[primary][0][0] == "pub" and signer in keys and keys[signer][1] == primary,
            "Tag signer is not bound to the approved primary public key.")
    for fingerprint in {primary, signer}:
        key = keys[fingerprint][0]
        require(key[1] not in ("r", "e", "d", "i", "n") and "D" not in key[11] and
                (not key[6] or int(key[6]) > time.time()), "Approved tag key is invalid, expired, revoked or disabled.")
    require("s" in keys[signer][0][11], "Approved tag key cannot sign.")


def signature_status(result, policy):
    require(result.returncode == 0, "git verify-tag rejected the signature; publication blocked.")
    statuses = [line.split() for line in result.stderr.decode("utf-8", errors="replace").splitlines() if line.startswith("[GNUPG:] ")]
    require(not any(len(fields) > 1 and fields[1] in BAD_STATUS for fields in statuses), "GPG reported an invalid/expired/revoked signature.")
    valid = [fields for fields in statuses if len(fields) > 1 and fields[1] == "VALIDSIG"]
    require(len(valid) == 1 and len(valid[0]) == 12, "Missing or ambiguous OpenPGP VALIDSIG status.")
    value = valid[0]
    require(value[2] in policy["allowed_signing_fingerprints"] and value[11] == policy["primary_fingerprint"], "Tag signer fingerprint is not authorized.")
    require(value[9] in ("8", "9", "10") and value[10] == "00", "Tag signature must use SHA-2 and the Git binary signature class.")
    require(value[5].isdigit() and (int(value[5]) == 0 or int(value[5]) > time.time()), "Tag signature has expired.")
    return value[2]


def verify_tag(repository, tag, commit):
    repository = Path(repository).resolve(strict=True)
    require(re.fullmatch(r"[0-9a-f]{40}", commit), "Invalid built source Commit.")
    require(git(repository, "rev-parse", "--verify", "HEAD^{commit}").stdout.decode().strip() == commit,
            "Built source Commit does not match checkout HEAD.")
    version = git(repository, "show", f"{commit}:VERSION").stdout.decode("utf-8").strip()
    require(re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:\.(?:0|[1-9][0-9]*))?-holybear", version)
            and tag == "v" + version, "Formal tag does not exactly match source VERSION.")
    reviewed = git(repository, "rev-parse", "--verify", f"{REVIEWED_REF}^{{commit}}").stdout.decode().strip()
    git(repository, "merge-base", "--is-ancestor", commit, reviewed)
    ref = "refs/tags/" + tag
    object_id = git(repository, "rev-parse", "--verify", ref).stdout.decode().strip()
    require(git(repository, "cat-file", "-t", object_id).stdout.strip() == b"tag", "Formal tag must be annotated, not lightweight.")
    payload = git(repository, "cat-file", "tag", object_id).stdout
    header = payload.split(b"\n\n", 1)[0].decode("utf-8").splitlines()
    require(len(header) >= 4 and header[0] == "object " + commit and header[1] == "type commit" and header[2] == "tag " + tag,
            "Annotated tag name or direct source Commit mismatch.")
    require(payload.count(b"\n-----BEGIN PGP SIGNATURE-----\n") == 1, "Annotated tag lacks a single OpenPGP signature.")
    # Pin trust to a resolved reviewed branch commit, never to tag-supplied files
    # or the runner's global keyring. These blobs are public material only.
    policy, public = trust_policy(repository, reviewed)
    if shutil.which("gpg") is None and os.name == "nt":
        executable = shutil.which("git")
        candidate = Path(executable).parent.parent / "usr/bin/gpg.exe" if executable else None
        require(candidate and candidate.is_file(), "Required GPG executable missing; publication blocked.")
        os.environ["PATH"] = str(candidate.parent) + os.pathsep + os.environ.get("PATH", "")
    executable, _, _ = signing.gpg_tools()
    with signing.keyring() as home:
        (home / "gpg.conf").write_text("no-auto-key-retrieve\nauto-key-locate clear\n", encoding="ascii")
        signing.gpg(home, ["--no-options", "--import"], public, stage="Reviewed public-key import")
        listing = signing.gpg(home, ["--no-options", "--with-colons", "--fixed-list-mode", "--list-keys"])
        env = dict(os.environ, GNUPGHOME=signing.gpg_path(home))
        result = git(repository, "-c", "gpg.format=openpgp", "-c", f"gpg.program={executable}",
                     "-c", f"gpg.openpgp.program={executable}", "verify-tag", "--raw", object_id, env=env, check=False)
        signer = signature_status(result, policy)
        usable_keys(listing, policy["primary_fingerprint"], signer)
    require(git(repository, "rev-parse", "--verify", ref).stdout.decode().strip() == object_id,
            "Tag ref changed during verification.")
    return {"tag": tag, "commit": commit, "tag_object": object_id, "signer": signer,
            "primary": policy["primary_fingerprint"], "reviewed_commit": reviewed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--commit", required=True)
    args = parser.parse_args()
    if os.environ.get("GITHUB_ACTIONS") == "true":
        require(signing.is_authorized_actions_repository(os.environ),
                "Tag verification is restricted to the authorized HolyBear Actions repository.")
    print("Verified approved OpenPGP source tag: " + json.dumps(verify_tag(args.repository, args.tag, args.commit), sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print("Formal tag rejected; publication blocked: " + (str(error) if isinstance(error, RuntimeError) else type(error).__name__))
        raise SystemExit(1)
