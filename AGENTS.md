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
- Formal CI and GitHub Releases require a complete unfiltered CTest suite. Release-labelled tests remain a focused subset, not a replacement for Full.
- For local acceptance or repackaging of an exact source commit, a successful unfiltered GitHub Actions Full run can satisfy the Full gate when code, build settings, tests, and required test resources are unchanged. Record the run as evidence, then perform a clean local build, Smoke or affected tests, and packaging validation. Do not rerun local Full solely because the same commit is being rebuilt.
- If matching CI Full evidence is absent or any relevant input changed, run Full locally.
- Keep CI and release scripts running the full suite and its existing completeness, signing, packaging, installer, and security checks. Do not add a Smoke-only filter to formal CI or release gates.
- Do not weaken assertions, skip tests, or remove tests to shorten a run.

## Avoid unnecessary reruns

- Documentation-only changes, Git commits, and fast-forward operations do not by themselves require a full CTest run.
- Do not run all 51 tests automatically for every small code change; use the affected tests and Smoke according to the scope above.

## Packaged settings readiness gate

- When settings HTML, script URLs, WebView policy or packaged UI resources change, build the isolated test hosts first and require settings_security, webview_runtime_security, webview_settings_layout, webview_f1_visibility and webview_missing_assets to pass before building or handing over the acceptance application.
- Check the actual packaged HTML script URLs against the native resource allowlist. A browser preview cannot establish native Ready bridge or F1 readiness. Never bypass the Ready guard or relax the allowlist to make this gate pass.
