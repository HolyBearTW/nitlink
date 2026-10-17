# NitLink development test policy

## Daily development

- Run the focused test or tests affected by a change first.
- For a general feature change, run the Smoke label and any affected tests not already in Smoke.
- For UI or localization changes, also run the relevant JavaScript checks, locale validation, and WebView tests.
- A Smoke pass is not a Full pass.
- When source code and relevant resources are unchanged, do not repeat the same tests without a reason.

Common commands from the repository root:
Smoke: ctest --test-dir out/release-x64-20261017 -C Release -L "^smoke$" --parallel 1 --output-on-failure
Full: ctest --test-dir out/release-x64-20261017 -C Release -L "^full$" --parallel 1 --output-on-failure
Release subset: ctest --test-dir out/release-x64-20261017 -C Release -L "^release$" --parallel 1 --output-on-failure

## High-risk changes and release

- For major cross-module changes, core architecture work, or high-risk security changes, choose the full CTest suite when the risk warrants it.
- Formal integration, release candidates, and GitHub Releases require the complete unfiltered CTest suite. Release-labelled tests are an additional focused subset, not a replacement for Full.
- Keep CI and release scripts running the full suite and its existing completeness, signing, packaging, installer, and security checks. Do not add a Smoke-only filter to formal CI or release gates.
- Do not weaken assertions, skip tests, or remove tests to shorten a run.

## Avoid unnecessary reruns

- Documentation-only changes, Git commits, and fast-forward operations do not by themselves require a full CTest run.
- Do not run all 51 tests automatically for every small code change; use the affected tests and Smoke according to the scope above.
