import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import '../tool/vendor_provenance_contract.dart';

void main() {
  test('discovers the Voice vendor configuration from its package', () async {
    final contracts = await loadVendorProvenanceContracts();

    final contract = contracts.singleWhere(
      (candidate) => candidate.package == 'flutter_webrtc',
    );
    expect(contract.name, 'Voice flutter_webrtc');
    expect(contract.version, '1.6.3');
    expect(
      contract.catalog,
      'packages/discourse_voice/tool/vendor_contract.json',
    );
    // Absolute filesystem paths use the platform's separators.
    String posix(String path) => path.replaceAll(r'\', '/');
    expect(
      posix(contract.patchManifest),
      endsWith(
        'packages/discourse_voice/third_party/flutter_webrtc/PATCHES.md',
      ),
    );
    expect(
      posix(contract.vendorPath),
      endsWith('packages/discourse_voice/third_party/flutter_webrtc'),
    );
  });

  group('Windows plugin coverage', () {
    // The Windows runner builds these files; a change to any of them must be
    // inventoried in PATCHES.md like every other vendored difference.
    const windowsPluginFiles = [
      'windows/CMakeLists.txt',
      'windows/flutter_webrtc_plugin.cc',
      'windows/task_runner_windows.cc',
      'third_party/CMakeLists.txt',
      'third_party/libwebrtc_version.ini',
      'common/cpp/src/flutter_webrtc.cc',
    ];

    test('the vendored tree read for comparison includes them', () async {
      final contract = (await loadVendorProvenanceContracts()).singleWhere(
        (candidate) => candidate.package == 'flutter_webrtc',
      );
      final files = await readDirectoryFiles(Directory(contract.vendorPath));

      for (final path in windowsPluginFiles) {
        expect(files, contains(path), reason: path);
      }
      expect(
        files.keys.where((path) => path.startsWith('windows/')),
        isNotEmpty,
      );
    });

    test('an undocumented edit to any of them fails the comparison', () {
      for (final path in windowsPluginFiles) {
        final comparison = compareVendorFiles(
          baseline: {path: _bytes('upstream')},
          vendor: {path: _bytes('patched')},
          documentedPatches: const {},
        );
        expect(comparison.matchesDocumentedInventory, isFalse, reason: path);
        expect(comparison.undocumentedDifferences, [path]);
      }
    });

    test('a documented edit passes', () {
      for (final path in windowsPluginFiles) {
        final comparison = compareVendorFiles(
          baseline: {path: _bytes('upstream')},
          vendor: {path: _bytes('patched')},
          documentedPatches: {path},
        );
        expect(comparison.matchesDocumentedInventory, isTrue, reason: path);
      }
    });

    test('the patch manifest inventories the Windows build manifest', () {
      final inventory = parseDocumentedPatchInventory(
        File(
          'packages/discourse_voice/third_party/flutter_webrtc/PATCHES.md',
        ).readAsStringSync(),
      );
      expect(inventory, contains('windows/CMakeLists.txt'));
      expect(inventory, contains('common/cpp/src/flutter_shared_screen.cc'));
    });

    test('the root Windows runner builds the vendored plugin', () {
      final plugins = File(
        'windows/flutter/generated_plugins.cmake',
      ).readAsStringSync();
      expect(plugins, contains('flutter_webrtc'));
      expect(File('windows/CMakeLists.txt').existsSync(), isTrue);
      expect(File('windows/runner/main.cpp').existsSync(), isTrue);
    });
  });

  group('readDirectoryFiles generated directories', () {
    late Directory root;

    Future<void> write(String path, [String content = 'x']) async {
      final file = File('${root.path}/$path');
      await file.parent.create(recursive: true);
      await file.writeAsString(content);
    }

    setUp(() async {
      root = await Directory.systemTemp.createTemp('vendor-read-');
      addTearDown(() => root.delete(recursive: true));
    });

    test('skips exactly the generated directories', () async {
      const generated = [
        '.dart_tool/package_config.json',
        'build/app.txt',
        'example/.dart_tool/package_config.json',
        'example/build/out.txt',
        'third_party/downloads/libwebrtc-win-x64-release.zip',
        'third_party/libwebrtc/lib/libwebrtc.dll',
      ];
      const sources = [
        'pubspec.yaml',
        'lib/build/real.dart',
        'example/lib/main.dart',
        'example/build.gradle',
        'windows/CMakeLists.txt',
        'common/cpp/src/flutter_webrtc.cc',
        'third_party/CMakeLists.txt',
        'third_party/libwebrtc_version.ini',
        'third_party/svpng/svpng.inc',
        'third_party/other/file.txt',
        'tool/.dart_tool/kept.txt',
      ];
      for (final path in [...generated, ...sources]) {
        await write(path);
      }

      final files = await readDirectoryFiles(root);

      expect(files.keys.toSet(), sources.toSet());
    });

    test(
      'prunes a generated directory before inspecting what is in it',
      () async {
        await write('pubspec.yaml');
        final outside = await Directory.systemTemp.createTemp('vendor-out-');
        addTearDown(() => outside.delete(recursive: true));
        await Directory('${root.path}/.dart_tool').create();
        await Link('${root.path}/.dart_tool/escape').create(outside.path);
        await Link('${root.path}/third_party').create(outside.path);

        // The link under .dart_tool is never looked at; the unrelated symlink
        // is still rejected, so pruning does not weaken the check elsewhere.
        await expectLater(readDirectoryFiles(root), throwsFormatException);
        await Link('${root.path}/third_party').delete();
        expect((await readDirectoryFiles(root)).keys, ['pubspec.yaml']);
      },
    );

    test('exclusions are exact paths, not names at any depth', () async {
      await write('lib/.dart_tool/a.txt');
      await write('third_party/downloads_extra/a.txt');
      await write('third_party/libwebrtc_version.ini');

      expect((await readDirectoryFiles(root)).keys.toSet(), {
        'lib/.dart_tool/a.txt',
        'third_party/downloads_extra/a.txt',
        'third_party/libwebrtc_version.ini',
      });
    });

    test('the generated list does not hide Windows build sources', () {
      for (final source in const [
        'windows/CMakeLists.txt',
        'common/cpp/src/flutter_webrtc.cc',
        'third_party/CMakeLists.txt',
        'third_party/libwebrtc_version.ini',
      ]) {
        expect(
          vendorGeneratedDirectories.any(
            (directory) =>
                source == directory || source.startsWith('$directory/'),
          ),
          isFalse,
          reason: source,
        );
      }
    });
  });

  test('rejects vendor paths which resolve outside the repository', () async {
    final repository = await Directory.systemTemp.createTemp(
      'vendor-contract-repository-',
    );
    addTearDown(() => repository.delete(recursive: true));
    final outside = await Directory.systemTemp.createTemp(
      'vendor-contract-outside-',
    );
    addTearDown(() => outside.delete(recursive: true));
    final tool = Directory('${repository.path}/packages/owner/tool');
    await tool.create(recursive: true);
    final vendor = Link('${repository.path}/packages/owner/third_party/vendor');
    await vendor.create(outside.path, recursive: true);
    final patches = File('${tool.path}/PATCHES.md');
    await patches.writeAsString('- `lib/file.dart`\n');
    await File('${tool.path}/vendor_contract.json').writeAsString(
      jsonEncode({
        'schemaVersion': 1,
        'name': 'escaped vendor',
        'package': 'escaped_vendor',
        'version': '1.0.0',
        'vendorPath': 'packages/owner/third_party/vendor',
        'patchManifest': 'packages/owner/tool/PATCHES.md',
      }),
    );

    expect(
      loadVendorProvenanceContracts(repository: repository),
      throwsFormatException,
    );
  });

  group('compareVendorFiles', () {
    test('accepts exactly the documented changed-file inventory', () {
      final comparison = compareVendorFiles(
        baseline: {
          'modified.txt': _bytes('upstream'),
          'unchanged.txt': _bytes('same'),
        },
        vendor: {
          'added.txt': _bytes('local'),
          'modified.txt': _bytes('patched'),
          'unchanged.txt': _bytes('same'),
        },
        documentedPatches: {'added.txt', 'modified.txt'},
      );

      expect(comparison.matchesDocumentedInventory, isTrue);
      expect(comparison.differences, {
        'added.txt': VendorDifferenceKind.added,
        'modified.txt': VendorDifferenceKind.modified,
      });
    });

    test('reports every undocumented added, modified, and removed file', () {
      final comparison = compareVendorFiles(
        baseline: {
          'modified.txt': _bytes('upstream'),
          'removed.txt': _bytes('upstream'),
        },
        vendor: {
          'added.txt': _bytes('local'),
          'modified.txt': _bytes('patched'),
        },
        documentedPatches: const {},
      );

      expect(comparison.matchesDocumentedInventory, isFalse);
      expect(comparison.undocumentedDifferences, [
        'added.txt',
        'modified.txt',
        'removed.txt',
      ]);
      expect(comparison.differences['added.txt'], VendorDifferenceKind.added);
      expect(
        comparison.differences['modified.txt'],
        VendorDifferenceKind.modified,
      );
      expect(
        comparison.differences['removed.txt'],
        VendorDifferenceKind.removed,
      );
    });

    test('rejects a documented file that no longer differs', () {
      final comparison = compareVendorFiles(
        baseline: {'restored.txt': _bytes('same')},
        vendor: {'restored.txt': _bytes('same')},
        documentedPatches: {'restored.txt'},
      );

      expect(comparison.matchesDocumentedInventory, isFalse);
      expect(comparison.undocumentedDifferences, isEmpty);
      expect(comparison.documentedFilesWithoutDifference, ['restored.txt']);
    });
  });

  test('reads only exact backticked file bullets as the patch inventory', () {
    const markdown = '''
- Archive SHA-256: `aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa`

Files:

- `lib/one.dart`
- `native/two.cc`

Run `dart run tool/vendor_provenance_contract.dart`.
''';

    expect(parseDocumentedPatchInventory(markdown), {
      'lib/one.dart',
      'native/two.cc',
    });
    expect(
      parseDocumentedArchiveSha256(markdown),
      'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    );
  });

  test('rejects duplicate documented patch files', () {
    const markdown = '''
- `lib/repeated.dart`
- `lib/repeated.dart`
''';

    expect(
      () => parseDocumentedPatchInventory(markdown),
      throwsFormatException,
    );
  });

  test('computes a lowercase SHA-256', () {
    expect(
      sha256Hex(utf8.encode('abc')),
      'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',
    );
  });
}

List<int> _bytes(String value) => utf8.encode(value);
