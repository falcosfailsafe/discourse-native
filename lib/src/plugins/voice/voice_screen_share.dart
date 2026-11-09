import 'package:flutter_webrtc/flutter_webrtc.dart' as rtc;

/// The sender's screen share quality choice for a shared hardware encode.
///
/// Peers never change it: the sender uploads [bitrate] to every viewer and
/// libwebrtc is told not to adapt it. See docs/plans/voice-hardware-encoding.md.
enum VoiceScreenSharePreset {
  /// 720p at 60 fps, for modest upload.
  p720x60(
    id: '720p60',
    maxHeight: 720,
    sourceScale: null,
    frameRate: 60,
    bitrate: 10000000,
  ),

  /// 1080p at 60 fps.
  p1080x60(
    id: '1080p60',
    maxHeight: 1080,
    sourceScale: null,
    frameRate: 60,
    bitrate: 16000000,
  ),

  /// Half of the source in each dimension at 60 fps, so a 7680×2160 desktop
  /// is sent as 3840×1080.
  halfSource60(
    id: 'half60',
    maxHeight: null,
    sourceScale: 0.5,
    frameRate: 60,
    bitrate: 25000000,
  );

  const VoiceScreenSharePreset({
    required this.id,
    required this.maxHeight,
    required this.sourceScale,
    required this.frameRate,
    required this.bitrate,
  });

  /// The default until the shipped presets are decided.
  static const defaultPreset = VoiceScreenSharePreset.p1080x60;

  /// Seconds between keyframes. A peer that drops frames freezes for up to
  /// this long, so it trades recovery time against keyframe cost.
  static const keyframeIntervalSeconds = 3;

  final String id;
  final int? maxHeight;
  final double? sourceScale;
  final int frameRate;

  /// Constant bitrate in bits per second, for one viewer.
  final int bitrate;

  /// The upload this preset needs for [viewers] peers, in whole megabits per
  /// second, rounded up. A mesh sender uploads one copy per viewer.
  int uploadMbps(int viewers) {
    final count = viewers < 1 ? 1 : viewers;
    return (bitrate * count + 999999) ~/ 1000000;
  }

  rtc.SharedScreenEncoderPreset toEncoderPreset() =>
      rtc.SharedScreenEncoderPreset(
        id: id,
        maxHeight: maxHeight,
        sourceScale: sourceScale,
        frameRate: frameRate,
        bitrate: bitrate,
        keyframeIntervalSeconds: keyframeIntervalSeconds,
      );
}

/// Hardware screen share controls a media session may offer.
///
/// Only sessions that can encode once for every peer implement it, so callers
/// reach it with `media is VoiceScreenShareEncoding`.
abstract interface class VoiceScreenShareEncoding {
  /// Whether starting a screen share would use the shared hardware encoder.
  ///
  /// Reported by the platform and memoized for the session. False wherever
  /// the encoder is missing, so the preset picker stays out of the way.
  Future<bool> hardwareEncodingAvailable();

  /// The preset the next shared-encode screen share uses.
  VoiceScreenSharePreset get screenSharePreset;
  set screenSharePreset(VoiceScreenSharePreset value);

  /// Peers whose connection did not negotiate H.264 High 5.1 or above, or
  /// could not be limited to the preset's rate, and so receive no screen
  /// video. The shared stream is H.264 whatever was negotiated, so sending it
  /// to them would corrupt, not degrade.
  Set<int> get screenShareUnsupportedPeerIds;

  /// Whether the local preview of [participantId] is withheld because its
  /// frames never reach the CPU, so a placeholder stands in for it.
  bool screenSharePreviewHidden(int participantId);
}

const _h264Mime = 'video/h264';
const _highProfileIdc = 0x64;

/// The first H.264 level that carries 3840×1080 at 60 fps (5.1 = 0x33).
const screenShareMinimumH264LevelIdc = 0x33;
const _maximumH264LevelIdc = 0x3e;

/// Codec preferences for the screen-share video transceiver: H.264 High at
/// level 5.1 or above first, highest level first, then every other codec so a
/// peer that cannot decode it still negotiates something.
///
/// Returns null when [capabilities] holds no such H.264 entry. The sender
/// never claims a codec its build does not list, so a build without High 5.1
/// keeps today's negotiation.
List<rtc.RTCRtpCodecCapability>? screenShareCodecPreferences(
  List<rtc.RTCRtpCodecCapability> capabilities,
) {
  final preferred = <(int, rtc.RTCRtpCodecCapability)>[];
  final rest = <rtc.RTCRtpCodecCapability>[];
  for (final codec in capabilities) {
    final level = _highProfileLevel(codec);
    if (level == null) {
      rest.add(codec);
    } else {
      preferred.add((level, codec));
    }
  }
  if (preferred.isEmpty) return null;
  // Stable: libwebrtc lists packetization-mode=1 before 0 per level already.
  final ordered = [
    for (final (_, codec) in _stableSortedByLevel(preferred)) codec,
  ];
  return [...ordered, ...rest];
}

List<(int, rtc.RTCRtpCodecCapability)> _stableSortedByLevel(
  List<(int, rtc.RTCRtpCodecCapability)> entries,
) {
  final indexed = [for (var i = 0; i < entries.length; i++) (i, entries[i])];
  indexed.sort((a, b) {
    final byLevel = b.$2.$1.compareTo(a.$2.$1);
    return byLevel != 0 ? byLevel : a.$1.compareTo(b.$1);
  });
  return [for (final (_, entry) in indexed) entry];
}

/// The `level_idc` of an H.264 High entry with packetization mode 1 at level
/// 5.1 or above, or null for any other entry.
int? _highProfileLevel(rtc.RTCRtpCodecCapability codec) =>
    screenShareH264Level(codec.mimeType, codec.sdpFmtpLine);

/// Whether a negotiated send codec is the H.264 the shared encoder emits.
bool isNegotiatedScreenShareCodec(rtc.RTCRTPCodec codec) {
  final parameters = codec.parameters;
  final fmtp = parameters == null
      ? null
      : [
          for (final entry in parameters.entries) '${entry.key}=${entry.value}',
        ].join(';');
  final name = codec.name ?? '';
  return screenShareH264Level(
        name.contains('/') ? name : 'video/$name',
        fmtp,
      ) !=
      null;
}

/// Parses an SDP media type and `a=fmtp` line; the `level_idc` when it is
/// H.264 High, packetization mode 1, level 5.1 up to 6.2.
int? screenShareH264Level(String mimeType, String? sdpFmtpLine) {
  if (mimeType.toLowerCase() != _h264Mime) return null;
  final parameters = <String, String>{};
  for (final part in (sdpFmtpLine ?? '').split(';')) {
    final index = part.indexOf('=');
    if (index <= 0) continue;
    parameters[part.substring(0, index).trim().toLowerCase()] = part
        .substring(index + 1)
        .trim();
  }
  if (parameters['packetization-mode'] != '1') return null;
  final id = parameters['profile-level-id'];
  if (id == null || id.length != 6) return null;
  final profile = int.tryParse(id.substring(0, 2), radix: 16);
  final level = int.tryParse(id.substring(4, 6), radix: 16);
  if (profile != _highProfileIdc || level == null) return null;
  if (level < screenShareMinimumH264LevelIdc || level > _maximumH264LevelIdc) {
    return null;
  }
  return level;
}
