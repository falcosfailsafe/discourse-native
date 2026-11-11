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

## Fixed since the first run (verified on Windows)

- Path separators in the byte-cache, private-staging, event-export,
  image-download and private-document tests.
- POSIX file-mode assertions are skipped on Windows; the two `linux_*` test
  files carry `@TestOn('!windows')`. The child-process test runs through the
  shell so `dart.bat` resolves.
- Source-scanning adoption tests normalise `\` to `/`.

## Failing on Linux too (not Windows problems)

`discourse_typography_adoption_test` (a `fontSize: 12` literal at
`global_search_panel.dart:1333`) and `control_style_adoption_test` fail on this
branch's base on Linux as well.

## Remaining, not fixed

- Golden tests (`control_consistency_golden_test`, topic-card and search
  goldens): pixel differences from Windows text rendering; they need their own
  goldens or a skip.
- Tests that feed POSIX paths (`/tmp/screen.mp4`, `file:///tmp/...`) into code
  that now sees Windows paths (composer upload panel, chat composer, upload
  preview).
- Layout/semantics tests with a 1 px difference, e.g. `a numeric value within
  0.05 of 52.05, actual 51.05` (notification menu mockup parity).
- `topic_title_field_ownership_test` (6): after a rename the shown title is
  the renamed one, not the translation. Passes on Linux; possibly a real
  Windows-specific ordering difference. Not investigated.
- `account_request_consumers` / `connection_session_integration`: "The forum
  session changed while loading its summary".
- `siteConfig reads the client settings route rather than /site.json`.
