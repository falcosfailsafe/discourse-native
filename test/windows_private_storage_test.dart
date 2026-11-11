import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:discourse_native/src/data/windows_private_storage.dart';
import 'package:discourse_native/src/foundation/windows_dpapi.dart';
import 'package:flutter_test/flutter_test.dart';

/// A reversible stand-in for DPAPI that, like DPAPI, produces different output
/// every time and rejects anything it did not produce.
final class _FakeProtection implements WindowsDataProtection {
  int _counter = 1;

  @override
  Uint8List protect(Uint8List data) {
    final salt = _counter++ & 0xff;
    final body = [for (final byte in data) byte ^ 0x5a ^ salt];
    final check = body.fold<int>(salt, (sum, b) => (sum + b) & 0xff);
    return Uint8List.fromList([0xd7, salt, ...body, check]);
  }

  @override
  Uint8List unprotect(Uint8List data) {
    if (data.length < 3 || data.first != 0xd7) {
      throw const FormatException('not a protected blob');
    }
    final salt = data[1];
    final body = data.sublist(2, data.length - 1);
    final check = body.fold<int>(salt, (sum, b) => (sum + b) & 0xff);
    if (check != data.last) throw const FormatException('blob is damaged');
    return Uint8List.fromList([for (final b in body) b ^ 0x5a ^ salt]);
  }
}

void main() {
  late Directory directory;
  late File file;

  WindowsPrivateStorage create([WindowsDataProtection? protection]) =>
      WindowsPrivateStorage(
        directory: directory,
        protection: protection ?? _FakeProtection(),
      );

  setUp(() {
    directory = Directory.systemTemp.createTempSync('windows-storage-test');
    file = File('${directory.path}/private-storage.dpapi');
  });

  tearDown(() => directory.deleteSync(recursive: true));

  group('WindowsPrivateStorage', () {
    test('stores, reads, lists and deletes values', () async {
      final storage = create();
      expect(await storage.read('a'), isNull);

      await storage.write('a', '1');
      await storage.write('b', '2');
      await storage.write('prefix/x', '3');
      await storage.write('prefix/y', '4');

      expect(await storage.read('a'), '1');
      expect(await storage.readAll(), {
        'a': '1',
        'b': '2',
        'prefix/x': '3',
        'prefix/y': '4',
      });

      await storage.delete('a');
      await storage.deletePrefix('prefix/');
      expect(await storage.readAll(), {'b': '2'});
    });

    test('a new instance sees what an earlier one wrote', () async {
      await create().write('token', 'hunter2-secret');
      expect(await create().read('token'), 'hunter2-secret');
    });

    test('holds nothing in plaintext on disk', () async {
      await create().write('discourse.example/api-key', 'hunter2-secret');

      final onDisk = file.readAsStringSync();
      expect(onDisk, isNot(contains('hunter2')));
      expect(onDisk, isNot(contains('api-key')));
      expect(onDisk, isNot(contains('{')));
      // It is base64 of a protected blob.
      expect(() => base64.decode(onDisk.trim()), returnsNormally);
    });

    test('does not rewrite an unchanged document', () async {
      final storage = create();
      await storage.write('a', '1');
      final first = file.readAsStringSync();

      await storage.write('a', '1');
      await storage.delete('missing');

      expect(file.readAsStringSync(), first);
    });

    test('rewrites when a value changes', () async {
      final storage = create();
      await storage.write('a', '1');
      final first = file.readAsStringSync();

      await storage.write('a', '2');

      expect(file.readAsStringSync(), isNot(first));
      expect(await storage.read('a'), '2');
    });

    test('a damaged file fails the read and is never replaced', () async {
      final storage = create();
      await storage.write('a', '1');
      final bytes = base64.decode(file.readAsStringSync().trim());
      bytes[bytes.length ~/ 2] ^= 0x01;
      file.writeAsStringSync(base64.encode(bytes));
      final damaged = file.readAsStringSync();

      await expectLater(storage.read('a'), throwsFormatException);
      await expectLater(storage.write('b', '2'), throwsFormatException);

      expect(file.readAsStringSync(), damaged);
    });

    test(
      'a file protected by another account is refused, not overwritten',
      () async {
        await create().write('a', '1');
        final theirs = file.readAsStringSync();

        final otherUser = _RefusingProtection();
        await expectLater(create(otherUser).read('a'), throwsFormatException);
        await expectLater(
          create(otherUser).write('b', '2'),
          throwsFormatException,
        );

        expect(file.readAsStringSync(), theirs);
      },
    );

    test('rejects a document of the wrong shape', () async {
      final protection = _FakeProtection();
      final wrong = jsonEncode({
        'version': 1,
        'values': {'a': 3},
      });
      file.writeAsStringSync(
        base64.encode(
          protection.protect(Uint8List.fromList(utf8.encode(wrong))),
        ),
      );

      await expectLater(create(protection).read('a'), throwsFormatException);
    });

    test('rejects an unknown format version', () async {
      final protection = _FakeProtection();
      final future = jsonEncode({'version': 99, 'values': <String, String>{}});
      file.writeAsStringSync(
        base64.encode(
          protection.protect(Uint8List.fromList(utf8.encode(future))),
        ),
      );

      await expectLater(create(protection).read('a'), throwsFormatException);
    });
  });

  group('WindowsDpapi', () {
    final windows = Platform.isWindows;
    final reason = windows ? null : 'DPAPI exists only on Windows';

    test('round-trips and does not leave plaintext', () {
      final dpapi = WindowsDpapi();
      final plain = Uint8List.fromList(utf8.encode('hunter2-secret'));

      final blob = dpapi.protect(plain);

      expect(
        utf8.decode(blob, allowMalformed: true),
        isNot(contains('hunter2')),
      );
      expect(dpapi.unprotect(blob), plain);
    }, skip: reason);

    test('protecting the same data twice gives different blobs', () {
      final dpapi = WindowsDpapi();
      final plain = Uint8List.fromList(utf8.encode('same'));
      expect(dpapi.protect(plain), isNot(dpapi.protect(plain)));
    }, skip: reason);

    test('handles empty and large data', () {
      final dpapi = WindowsDpapi();
      expect(dpapi.unprotect(dpapi.protect(Uint8List(0))), isEmpty);
      final large = Uint8List.fromList(List.generate(1 << 20, (i) => i & 0xff));
      expect(dpapi.unprotect(dpapi.protect(large)), large);
    }, skip: reason);

    test('refuses a damaged or foreign blob', () {
      final dpapi = WindowsDpapi();
      final blob = dpapi.protect(Uint8List.fromList(utf8.encode('x')));
      blob[blob.length ~/ 2] ^= 0xff;
      expect(() => dpapi.unprotect(blob), throwsFormatException);
      expect(
        () => dpapi.unprotect(Uint8List.fromList([1, 2, 3])),
        throwsFormatException,
      );
    }, skip: reason);

    test('backs a real storage file', () async {
      final storage = WindowsPrivateStorage(directory: directory);
      await storage.write('token', 'hunter2-secret');
      expect(file.readAsStringSync(), isNot(contains('hunter2')));
      expect(
        await WindowsPrivateStorage(directory: directory).read('token'),
        'hunter2-secret',
      );
    }, skip: reason);
  });
}

final class _RefusingProtection implements WindowsDataProtection {
  @override
  Uint8List protect(Uint8List data) => data;

  @override
  Uint8List unprotect(Uint8List data) =>
      throw const FormatException('protected by another account');
}
