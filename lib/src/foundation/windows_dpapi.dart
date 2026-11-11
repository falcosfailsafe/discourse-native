import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

/// Per-user encryption of small blobs. Windows implements it with DPAPI
/// ([WindowsDpapi]); tests substitute a reversible fake so the storage built on
/// top can be exercised on any platform.
abstract interface class WindowsDataProtection {
  Uint8List protect(Uint8List data);

  /// Throws [FormatException] when [data] is not a blob this user can open
  /// (corrupted, or protected by another user or machine).
  Uint8List unprotect(Uint8List data);
}

final class _DataBlob extends Struct {
  @Uint32()
  external int cbData;

  external Pointer<Uint8> pbData;
}

typedef _ProtectNative =
    Int32 Function(
      Pointer<_DataBlob>,
      Pointer<Utf16>,
      Pointer<_DataBlob>,
      Pointer<Void>,
      Pointer<Void>,
      Uint32,
      Pointer<_DataBlob>,
    );
typedef _ProtectDart =
    int Function(
      Pointer<_DataBlob>,
      Pointer<Utf16>,
      Pointer<_DataBlob>,
      Pointer<Void>,
      Pointer<Void>,
      int,
      Pointer<_DataBlob>,
    );
typedef _UnprotectNative =
    Int32 Function(
      Pointer<_DataBlob>,
      Pointer<Pointer<Utf16>>,
      Pointer<_DataBlob>,
      Pointer<Void>,
      Pointer<Void>,
      Uint32,
      Pointer<_DataBlob>,
    );
typedef _UnprotectDart =
    int Function(
      Pointer<_DataBlob>,
      Pointer<Pointer<Utf16>>,
      Pointer<_DataBlob>,
      Pointer<Void>,
      Pointer<Void>,
      int,
      Pointer<_DataBlob>,
    );
typedef _LocalFreeNative = Pointer<Void> Function(Pointer<Void>);
typedef _LocalFreeDart = Pointer<Void> Function(Pointer<Void>);

/// CRYPTPROTECT_UI_FORBIDDEN: never show a prompt.
const int _uiForbidden = 0x1;

/// Mixed into every blob so another program running as the same user cannot
/// open ours by calling DPAPI with no entropy.
final Uint8List _entropy = Uint8List.fromList(
  utf8.encode('org.discourse.native.private-storage.v1'),
);

/// Windows Data Protection API, scoped to the current user.
///
/// The DLLs are opened on first use, so importing this on another platform is
/// harmless as long as nothing calls it.
final class WindowsDpapi implements WindowsDataProtection {
  WindowsDpapi();

  late final DynamicLibrary _crypt32 = DynamicLibrary.open('crypt32.dll');
  late final DynamicLibrary _kernel32 = DynamicLibrary.open('kernel32.dll');
  late final _ProtectDart _protect = _crypt32
      .lookupFunction<_ProtectNative, _ProtectDart>('CryptProtectData');
  late final _UnprotectDart _unprotect = _crypt32
      .lookupFunction<_UnprotectNative, _UnprotectDart>('CryptUnprotectData');
  late final _LocalFreeDart _localFree = _kernel32
      .lookupFunction<_LocalFreeNative, _LocalFreeDart>('LocalFree');

  @override
  Uint8List protect(Uint8List data) => _transform(data, protect: true);

  @override
  Uint8List unprotect(Uint8List data) => _transform(data, protect: false);

  Uint8List _transform(Uint8List data, {required bool protect}) {
    if (!Platform.isWindows) {
      throw UnsupportedError('DPAPI is only available on Windows');
    }
    final input = calloc<_DataBlob>();
    final entropy = calloc<_DataBlob>();
    final output = calloc<_DataBlob>();
    final inputBytes = malloc<Uint8>(data.isEmpty ? 1 : data.length);
    final entropyBytes = malloc<Uint8>(_entropy.length);
    try {
      inputBytes.asTypedList(data.length).setAll(0, data);
      entropyBytes.asTypedList(_entropy.length).setAll(0, _entropy);
      input.ref
        ..cbData = data.length
        ..pbData = inputBytes;
      entropy.ref
        ..cbData = _entropy.length
        ..pbData = entropyBytes;
      final ok = protect
          ? _protect(
              input,
              nullptr,
              entropy,
              nullptr,
              nullptr,
              _uiForbidden,
              output,
            )
          : _unprotect(
              input,
              nullptr,
              entropy,
              nullptr,
              nullptr,
              _uiForbidden,
              output,
            );
      if (ok == 0) {
        if (protect) throw StateError('DPAPI could not protect the data');
        throw const FormatException('DPAPI could not open the data');
      }
      final result = Uint8List.fromList(
        output.ref.pbData.asTypedList(output.ref.cbData),
      );
      _localFree(output.ref.pbData.cast());
      return result;
    } finally {
      malloc.free(inputBytes);
      malloc.free(entropyBytes);
      calloc.free(input);
      calloc.free(entropy);
      calloc.free(output);
    }
  }
}
