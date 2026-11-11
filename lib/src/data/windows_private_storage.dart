import 'dart:collection';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:discourse_native/l10n/strings.dart';

import '../foundation/private_file_document.dart';
import '../foundation/windows_dpapi.dart';
import 'private_storage.dart';

/// Credential and draft storage on Windows: one JSON document encrypted with the
/// current user's DPAPI key, written as base64 to
/// `%LOCALAPPDATA%\discourse-native\private-storage.dpapi`.
///
/// DPAPI ties the blob to the Windows account, so the file is unreadable
/// to other users and on other machines, and it holds nothing in plaintext.
/// The directory is under the user's profile, whose default permissions already
/// exclude other users. Updates are flushed to a same-directory temporary file
/// before an atomic rename, so an interruption cannot leave half a document.
///
/// A file that cannot be opened (corrupted, or protected by another account)
/// fails its transaction with a [FormatException] and is never replaced.
final class WindowsPrivateStorage implements EnumerablePrivateStorage {
  WindowsPrivateStorage({
    Directory? directory,
    WindowsDataProtection? protection,
  }) : _providedDirectory = directory,
       _protection = protection ?? WindowsDpapi();

  static const _fileName = 'private-storage.dpapi';
  static const _formatVersion = 1;

  final Directory? _providedDirectory;
  final WindowsDataProtection _protection;

  late final PrivateFileDocument<Map<String, String>> _document =
      PrivateFileDocument(
        target: _file,
        empty: () => <String, String>{},
        decode: _decode,
        encode: _encode,
        // Encryption is randomized, so unchanged documents must be recognised
        // by their content and not by their ciphertext.
        snapshot: (values) =>
            jsonEncode(SplayTreeMap<String, String>.of(values)),
      );

  @override
  Future<String?> read(String key) =>
      _document.read((values) => PrivateFileResult(values[key]));

  @override
  Future<Map<String, String>> readAll() =>
      _document.read((values) => PrivateFileResult(Map.unmodifiable(values)));

  @override
  Future<void> write(String key, String value) => _document.update((values) {
    values[key] = value;
    return PrivateFileResult.done;
  });

  @override
  Future<void> delete(String key) => _document.update((values) {
    values.remove(key);
    return PrivateFileResult.done;
  });

  @override
  Future<void> deletePrefix(String prefix) => _document.update((values) {
    values.removeWhere((key, _) => key.startsWith(prefix));
    return PrivateFileResult.done;
  });

  Future<File> _file() async {
    final provided = _providedDirectory;
    if (provided != null) return File('${provided.path}/$_fileName');
    final base =
        Platform.environment['LOCALAPPDATA'] ?? Platform.environment['APPDATA'];
    if (base == null || base.isEmpty) {
      throw StateError('LOCALAPPDATA does not name a directory');
    }
    return File('$base\\discourse-native\\$_fileName');
  }

  Map<String, String> _decode(String contents) {
    final Uint8List plain;
    try {
      plain = _protection.unprotect(base64.decode(contents.trim()));
    } on FormatException catch (error) {
      throw FormatException(
        appL10n.invalidPrivateStorage(error.message.toString()),
      );
    }
    final Object? decoded;
    try {
      decoded = jsonDecode(utf8.decode(plain));
    } on FormatException catch (error) {
      throw FormatException(
        appL10n.invalidPrivateStorage(error.message.toString()),
      );
    }
    if (decoded case {
      'version': final int version,
      'values': final Map<Object?, Object?> rawValues,
    } when version == _formatVersion) {
      final values = <String, String>{};
      for (final MapEntry(:key, :value) in rawValues.entries) {
        if (key is! String || value is! String) {
          throw FormatException(
            appL10n.invalidPrivateStorageValuesMustBeStrings,
          );
        }
        values[key] = value;
      }
      return values;
    }
    throw FormatException(appL10n.invalidPrivateStorageFormat);
  }

  String _encode(Map<String, String> values) {
    final json = utf8.encode(
      jsonEncode({'version': _formatVersion, 'values': values}),
    );
    return base64.encode(_protection.protect(Uint8List.fromList(json)));
  }
}
