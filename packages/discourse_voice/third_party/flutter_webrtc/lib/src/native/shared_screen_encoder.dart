import 'dart:async';

import 'package:flutter/services.dart';
import 'package:webrtc_interface/webrtc_interface.dart';

import 'media_stream_impl.dart';
import 'rtc_peerconnection_impl.dart';
import 'utils.dart';

/// Native error code meaning the shared screen encoder cannot run here, even
/// though [SharedScreenEncoder.capabilities] may have reported otherwise (the
/// GPU or driver went away, the encoder session could not be opened). Callers
/// may fall back to ordinary screen capture; every other failure, such as a
/// cancelled picker, must not be retried that way.
const sharedScreenUnavailableErrorCode = 'sharedScreenUnavailable';

/// Thrown by [SharedScreenEncoder.getSharedScreenMedia] when the native side
/// reports [sharedScreenUnavailableErrorCode].
class SharedScreenEncoderUnavailable implements Exception {
  const SharedScreenEncoderUnavailable([this.reason]);

  final String? reason;

  @override
  String toString() => 'SharedScreenEncoderUnavailable(${reason ?? ''})';
}

/// What the native shared screen encoder can do on this machine.
///
/// Reported by the platform, never inferred: [unavailable] is the answer on
/// every platform without the native method.
class SharedScreenEncoderCapabilities {
  const SharedScreenEncoderCapabilities({
    required this.available,
    this.codecs = const [],
    this.maxWidth,
    this.maxHeight,
    this.maxFrameRate,
    this.reason,
  });

  static const unavailable = SharedScreenEncoderCapabilities(available: false);

  factory SharedScreenEncoderCapabilities.fromMap(Map<dynamic, dynamic> map) {
    final codecs = map['codecs'];
    int? integer(Object? value) => value is num ? value.toInt() : null;
    final available = map['available'] == true;
    final parsedCodecs = [
      if (codecs is List)
        for (final codec in codecs)
          if (codec is String && codec.isNotEmpty) codec.toUpperCase(),
    ];
    // An encoder that names no codec cannot be asked for anything.
    if (!available || parsedCodecs.isEmpty) {
      return SharedScreenEncoderCapabilities(
        available: false,
        reason: map['reason'] is String ? map['reason'] as String : null,
      );
    }
    return SharedScreenEncoderCapabilities(
      available: true,
      codecs: parsedCodecs,
      maxWidth: integer(map['maxWidth']),
      maxHeight: integer(map['maxHeight']),
      maxFrameRate: integer(map['maxFrameRate']),
    );
  }

  /// Whether a shared NVENC session can be opened.
  final bool available;

  /// Upper-case codec names the encoder can produce, e.g. `H264`.
  final List<String> codecs;
  final int? maxWidth;
  final int? maxHeight;
  final int? maxFrameRate;

  /// Why the encoder is unavailable, when the platform said.
  final String? reason;

  bool supports(String codec) => codecs.contains(codec.toUpperCase());
}

/// Settings fixed by the sender for one shared-encode screen share.
///
/// Peers never change them. Exactly one of [maxHeight] and [sourceScale]
/// sizes the output: [maxHeight] fits the source into that height, keeping its
/// aspect ratio; [sourceScale] multiplies the source's own size.
class SharedScreenEncoderPreset {
  const SharedScreenEncoderPreset({
    required this.id,
    required this.frameRate,
    required this.bitrate,
    required this.keyframeIntervalSeconds,
    this.maxHeight,
    this.sourceScale,
    this.codec = 'H264',
  })  : assert((maxHeight == null) != (sourceScale == null)),
        assert(frameRate > 0),
        assert(bitrate > 0),
        assert(keyframeIntervalSeconds > 0);

  final String id;
  final int? maxHeight;
  final double? sourceScale;
  final int frameRate;

  /// Constant bitrate in bits per second.
  final int bitrate;
  final int keyframeIntervalSeconds;
  final String codec;

  Map<String, dynamic> toMap() => <String, dynamic>{
        'id': id,
        if (maxHeight != null) 'maxHeight': maxHeight,
        if (sourceScale != null) 'sourceScale': sourceScale,
        'frameRate': frameRate,
        'bitrate': bitrate,
        'keyframeIntervalSeconds': keyframeIntervalSeconds,
        'codec': codec,
      };
}

/// Starts screen shares whose frames stay on the GPU and are encoded once by
/// a shared hardware encoder, for every peer connection to send.
class SharedScreenEncoder {
  const SharedScreenEncoder._();

  /// Asks the platform whether a shared encoder is available.
  ///
  /// Only Windows implements it. Anywhere else, and whenever the platform
  /// does not answer, the result is [SharedScreenEncoderCapabilities.unavailable]
  /// so callers keep today's capture path.
  static Future<SharedScreenEncoderCapabilities> capabilities() async {
    if (!WebRTC.platformIsWindows) {
      return SharedScreenEncoderCapabilities.unavailable;
    }
    try {
      final response = await WebRTC.invokeMethod<Map<dynamic, dynamic>, void>(
        'getSharedScreenEncoderCapabilities',
        <String, dynamic>{},
      );
      if (response == null) return SharedScreenEncoderCapabilities.unavailable;
      return SharedScreenEncoderCapabilities.fromMap(response);
    } on MissingPluginException {
      return SharedScreenEncoderCapabilities.unavailable;
    } on PlatformException {
      return SharedScreenEncoderCapabilities.unavailable;
    }
  }

  /// Starts a shared-encode screen share and returns its stream.
  ///
  /// [sourceId] names the monitor to capture; omitted or empty, native uses
  /// the primary monitor without showing a picker.
  ///
  /// The video track carries already-encoded frames; it has no CPU frames to
  /// preview. Throws [SharedScreenEncoderUnavailable] when the encoder cannot
  /// run, and any other error unchanged.
  static Future<MediaStream> getSharedScreenMedia(
    SharedScreenEncoderPreset preset, {
    bool audio = true,
    String? sourceId,
  }) async {
    try {
      final response = await WebRTC.invokeMethod(
        'getSharedScreenMedia',
        <String, dynamic>{
          'preset': preset.toMap(),
          'audio': audio,
          if (sourceId != null && sourceId.isNotEmpty) 'sourceId': sourceId,
        },
      );
      if (response == null) {
        throw Exception('getSharedScreenMedia return null, something wrong');
      }
      final stream = MediaStreamNative(response['streamId'], 'local');
      stream.setMediaTracks(
          response['audioTracks'] ?? [], response['videoTracks'] ?? []);
      return stream;
    } on MissingPluginException catch (e) {
      throw SharedScreenEncoderUnavailable(e.message);
    } on PlatformException catch (e) {
      if (e.code == sharedScreenUnavailableErrorCode) {
        throw SharedScreenEncoderUnavailable(e.message);
      }
      throw 'Unable to getSharedScreenMedia: ${e.message}';
    }
  }

  /// Sets the bandwidth estimator's minimum, start and maximum bitrates, in
  /// bits per second, for one peer connection.
  ///
  /// The shared encoder sends a fixed rate; with libwebrtc's defaults the
  /// estimate starts at a few hundred kbps and the peer freezes repeatedly
  /// while it climbs. Native maps this to
  /// `PeerConnectionInterface::SetBitrate(BitrateSettings{min, start, max})`.
  ///
  /// Throws when the peer is not a native peer connection or the platform
  /// refuses, so the caller never assumes the estimator was raised.
  static Future<void> setPeerBitrate(
    RTCPeerConnection peer, {
    required int minBitrate,
    required int startBitrate,
    required int maxBitrate,
  }) async {
    if (peer is! RTCPeerConnectionNative) {
      throw const SharedScreenEncoderUnavailable('not a native peer');
    }
    try {
      final response = await WebRTC.invokeMethod(
        'setPeerBitrate',
        <String, dynamic>{
          'peerConnectionId': peer.peerConnectionId,
          'minBitrate': minBitrate,
          'startBitrate': startBitrate,
          'maxBitrate': maxBitrate,
        },
      );
      if (response is! Map || response['result'] != true) {
        throw StateError('setPeerBitrate was not applied');
      }
    } on MissingPluginException catch (e) {
      throw SharedScreenEncoderUnavailable(e.message);
    } on PlatformException catch (e) {
      throw 'Unable to setPeerBitrate: ${e.message}';
    }
  }
}
