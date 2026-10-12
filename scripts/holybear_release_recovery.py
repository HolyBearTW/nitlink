"""One-version recovery contract; source tags and GitHub event metadata stay intact."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TAG = "v1.2.4.1-holybear"
TAG_OBJECT = "fffadac90dcf48690956fbf42c3fb66df1350a0c"
SOURCE_COMMIT = "af98e1eccc6835b9661555501c5e9bf63da493ec"
CONTROL_REF = "refs/heads/holybear/integrated"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def git(root, *args):
    env = {key: value for key, value in os.environ.items() if key not in
           ("HOLYBEAR_GPG_PRIVATE_KEY", "HOLYBEAR_GPG_PASSPHRASE", "HOLYBEAR_GPG_FINGERPRINT")}
    env["GIT_NO_REPLACE_OBJECTS"] = "1"
    result = subprocess.run(["git", "-C", str(root), *args], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    require(result.returncode == 0, "Recovery Git source/ref check failed.")
    return result.stdout.decode("utf-8").strip()


def require_recovery():
    require(os.environ.get("GITHUB_ACTIONS") == "true" and
            os.environ.get("GITHUB_REPOSITORY") == "HolyBearTW/nitlink" and
            os.environ.get("GITHUB_EVENT_NAME") == "workflow_dispatch" and
            os.environ.get("GITHUB_REF") == CONTROL_REF and
            os.environ.get("GITHUB_WORKFLOW_REF") ==
            "HolyBearTW/nitlink/.github/workflows/holy-bear-release.yml@" + CONTROL_REF and
            os.environ.get("RECOVER_1241") == "true",
            "Recovery requires the explicit existing workflow dispatch on holybear/integrated.")
    require(Path(os.environ["GITHUB_WORKSPACE"]).resolve() == ROOT,
            "Recovery control checkout is not the workflow workspace.")
    control_commit = git(ROOT, "rev-parse", "HEAD")
    require(control_commit == os.environ.get("GITHUB_SHA") and
            not git(ROOT, "status", "--porcelain", "--untracked-files=no"),
            "Recovery control Commit or tracked code changed.")
    git(ROOT, "merge-base", "--is-ancestor", control_commit, "origin/holybear/integrated")
    source = (ROOT / "recovery-source").resolve(strict=True)
    require(not (ROOT / "recovery-source").is_symlink() and
            Path(git(source, "rev-parse", "--show-toplevel")).resolve() == source,
            "Recovery requires a separate source Git checkout.")
    require(git(source, "rev-parse", "HEAD") == SOURCE_COMMIT and
            git(source, "rev-parse", "refs/tags/" + TAG) == TAG_OBJECT and
            git(source, "cat-file", "-t", TAG_OBJECT) == "tag" and
            git(source, "rev-parse", "refs/tags/" + TAG + "^{commit}") == SOURCE_COMMIT and
            git(source, "show", "HEAD:VERSION") == "1.2.4.1-holybear" and
            not git(source, "status", "--porcelain", "--untracked-files=no"),
            "Recovery source must be the unchanged approved annotated Tag and Commit.")
    git(source, "merge-base", "--is-ancestor", SOURCE_COMMIT, "origin/holybear/integrated")
    remote = dict(line.split()[::-1] for line in git(source, "ls-remote", "origin",
                  "refs/tags/" + TAG, "refs/tags/" + TAG + "^{}").splitlines())
    require(remote == {"refs/tags/" + TAG: TAG_OBJECT, "refs/tags/" + TAG + "^{}": SOURCE_COMMIT},
            "Recovery remote Tag changed.")
    return {"tag": TAG, "tag_object": TAG_OBJECT, "source_commit": SOURCE_COMMIT,
            "source_ref": "refs/tags/" + TAG, "source_root": str(source),
            "control_commit": control_commit, "control_ref": CONTROL_REF}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-release-absent", action="store_true")
    args = parser.parse_args()
    identity = require_recovery()
    if args.check_release_absent:
        result = subprocess.run(["gh", "release", "list", "--repo", "HolyBearTW/nitlink",
                                 "--limit", "1000", "--json", "tagName"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        require(result.returncode == 0, "Cannot check existing releases; recovery blocked.")
        require(not any(release["tagName"] == TAG for release in json.loads(result.stdout)),
                "Recovery already has a Draft/Release; refusing another publication attempt.")
    print(json.dumps(identity, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print("Recovery blocked: " + (str(error) if isinstance(error, RuntimeError) else type(error).__name__))
        raise SystemExit(1)
