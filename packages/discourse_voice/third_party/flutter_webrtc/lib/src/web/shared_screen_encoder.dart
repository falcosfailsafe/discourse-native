import 'package:webrtc_interface/webrtc_interface.dart';

const sharedScreenUnavailableErrorCode = 'sharedScreenUnavailable';

class SharedScreenEncoderUnavailable implements Exception {
  const SharedScreenEncoderUnavailable([this.reason]);

  final String? reason;

  @override
  String toString() => 'SharedScreenEncoderUnavailable(${reason ?? ''})';
}

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

  final bool available;
  final List<String> codecs;
  final int? maxWidth;
  final int? maxHeight;
  final int? maxFrameRate;
  final String? reason;

  bool supports(String codec) => false;
}

class SharedScreenEncoderPreset {
  const SharedScreenEncoderPreset({
    required this.id,
    required this.frameRate,
    required this.bitrate,
    required this.keyframeIntervalSeconds,
    this.maxHeight,
    this.sourceScale,
    this.codec = 'H264',
  });

  final String id;
  final int? maxHeight;
  final double? sourceScale;
  final int frameRate;
  final int bitrate;
  final int keyframeIntervalSeconds;
  final String codec;

  Map<String, dynamic> toMap() => <String, dynamic>{'id': id};
}

/// The browser has no shared screen encoder.
class SharedScreenEncoder {
  const SharedScreenEncoder._();

  static Future<SharedScreenEncoderCapabilities> capabilities() async =>
      SharedScreenEncoderCapabilities.unavailable;

  static Future<MediaStream> getSharedScreenMedia(
    SharedScreenEncoderPreset preset, {
    bool audio = true,
    String? sourceId,
  }) =>
      throw const SharedScreenEncoderUnavailable('web');

  static Future<void> setPeerBitrate(
    RTCPeerConnection peer, {
    required int minBitrate,
    required int startBitrate,
    required int maxBitrate,
  }) =>
      throw const SharedScreenEncoderUnavailable('web');
}
