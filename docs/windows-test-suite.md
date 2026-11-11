# App test suite on Windows

Run on Windows Server 2025 (Tesla T4 box), `flutter test --concurrency=4`, at
the `fix/windows-private-storage` branch plus the transport fix below.

Result before fixes: **27,137 tests, 109 failed, 19 skipped** (one full run,
about 61 minutes). On Linux the same tests pass.

## Real Windows bugs found

- **Failed-write classification.** `DiscourseTransport._neverSent` knew only
  macOS and Linux errno numbers, so a refused connection on Windows was
  reported as "may have been sent". Dart on Windows raises Win32 codes
  (refused = 1225, verified; 1231/1232 unreachable are from the Win32 table,
  not reproduced). Fixed; the real-socket test passes on Windows.
- **No credential/draft storage on Windows** (35 failures): fixed on
  `fix/windows-private-storage` (DPAPI).

## Test-side assumptions (not app bugs, not yet fixed)

- POSIX file modes (`chmod`, 0600/0700 expectations): about 10 tests in the
  preferences, private-file and directory-restriction suites.
- Path separators (`/` vs `\`): about 12 tests (event export, image download,
  byte cache scans).

## Not triaged

About 50 UI tests fail with only "Test failed. See exception logs above":
control-family goldens, composer/chat drop and upload, rail actions, topic
sheets, user menu. They need individual reading of the logs. Source-scanning
lint tests (`font-size literals`, `UI kit`) also fail on `\` in paths.
Also failing: `siteConfig reads the client settings route rather than
/site.json` (an extra `/site.json` request).
