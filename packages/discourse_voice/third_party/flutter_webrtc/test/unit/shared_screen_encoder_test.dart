import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:flutter_webrtc/src/native/rtc_peerconnection_impl.dart';
import 'package:flutter_webrtc/src/native/shared_screen_encoder.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('FlutterWebRTC.Method');
  final calls = <MethodCall>[];
  Future<Object?> Function(MethodCall)? handler;

  setUp(() {
    calls.clear();
    handler = null;
    channel.setMockMethodCallHandler((call) async {
      if (call.method == 'initialize') return null;
      calls.add(call);
      return handler?.call(call);
    });
  });
  tearDown(() => channel.setMockMethodCallHandler(null));

  const preset = SharedScreenEncoderPreset(
    id: 'half60',
    sourceScale: 0.5,
    frameRate: 60,
    bitrate: 25000000,
    keyframeIntervalSeconds: 3,
  );

  test('preset serialises only its sizing rule', () {
    expect(preset.toMap(), {
      'id': 'half60',
      'sourceScale': 0.5,
      'frameRate': 60,
      'bitrate': 25000000,
      'keyframeIntervalSeconds': 3,
      'codec': 'H264',
    });
  });

  test('capabilities parse the platform answer', () {
    final caps = SharedScreenEncoderCapabilities.fromMap({
      'available': true,
      'codecs': ['h264'],
      'maxWidth': 3840,
    });
    expect(caps.available, isTrue);
    expect(caps.supports('H264'), isTrue);
    expect(caps.supports('AV1'), isFalse);
    expect(caps.maxWidth, 3840);
  });

  test('a peer bitrate request that native does not confirm throws', () async {
    final peer = RTCPeerConnectionNative('pc1', {});
    handler = (_) async => {'result': false};
    await expectLater(
      SharedScreenEncoder.setPeerBitrate(
        peer,
        minBitrate: 1,
        startBitrate: 2,
        maxBitrate: 3,
      ),
      throwsStateError,
    );
    expect(calls.single.method, 'setPeerBitrate');
    expect(calls.single.arguments, {
      'peerConnectionId': 'pc1',
      'minBitrate': 1,
      'startBitrate': 2,
      'maxBitrate': 3,
    });
  });

  test('a missing native method reads as unavailable', () async {
    final peer = RTCPeerConnectionNative('pc1', {});
    handler = (_) async => throw MissingPluginException();
    await expectLater(
      SharedScreenEncoder.setPeerBitrate(
        peer,
        minBitrate: 1,
        startBitrate: 2,
        maxBitrate: 3,
      ),
      throwsA(isA<SharedScreenEncoderUnavailable>()),
    );
  });

  test('capabilities are unavailable off Windows without a native call',
      () async {
    final caps = await SharedScreenEncoder.capabilities();
    expect(caps.available, isFalse);
    expect(calls, isEmpty);
  });

  test('sharedScreenUnavailable error maps to the fallback exception',
      () async {
    handler = (_) async =>
        throw PlatformException(code: sharedScreenUnavailableErrorCode);
    await expectLater(
      SharedScreenEncoder.getSharedScreenMedia(preset),
      throwsA(isA<SharedScreenEncoderUnavailable>()),
    );
  });

  test('other native errors are final, not a fallback', () async {
    handler = (_) async => throw PlatformException(code: 'cancelled');
    await expectLater(
      SharedScreenEncoder.getSharedScreenMedia(preset),
      throwsA(isNot(isA<SharedScreenEncoderUnavailable>())),
    );
  });

  test('getSharedScreenMedia sends the preset and returns the stream',
      () async {
    handler = (_) async => {
          'streamId': 's1',
          'audioTracks': [],
          'videoTracks': [
            {
              'id': 'v1',
              'label': 'screen',
              'kind': 'video',
              'enabled': true,
            },
          ],
        };
    final stream = await SharedScreenEncoder.getSharedScreenMedia(preset);
    expect(stream.id, 's1');
    expect(stream.getVideoTracks().single.id, 'v1');
    expect(calls.single.arguments, {'preset': preset.toMap(), 'audio': true});
  });

  test('sourceId is sent only when given', () async {
    handler = (_) async => {
          'streamId': 's1',
          'audioTracks': [],
          'videoTracks': [],
        };
    await SharedScreenEncoder.getSharedScreenMedia(preset, sourceId: '');
    await SharedScreenEncoder.getSharedScreenMedia(preset, sourceId: 'mon2');
    expect((calls[0].arguments as Map).containsKey('sourceId'), isFalse);
    expect((calls[1].arguments as Map)['sourceId'], 'mon2');
  });
}
