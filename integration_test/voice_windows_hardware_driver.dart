// Windows hardware driver for the Voice shared-encode screen share.
//
// Not a `flutter test`: it is an entry point that is built and then run on a
// Windows machine that has an NVIDIA GPU and a display in the interactive
// session (see tool/libwebrtc/native_tests/windows/README.md):
//
//   flutter build windows --release --target integration_test/voice_windows_hardware_driver.dart
//   <runner>\discourse_native.exe            (from the console session)
//
// It runs the real [MeshVoiceMediaSession] with the real flutter_webrtc plugin
// and the pinned libwebrtc.dll: one sender and several receivers in one
// process, joined by an in-memory signaling bus. It checks what the user would
// see: the shared share starts through NVENC, every receiver decodes video at
// the preset's size and rate, one NVENC session serves them all, a participant
// leaving or the share stopping and restarting leaves everyone else alone, and
// a sender without the encoder still shares through today's path.
//
// Progress and results are written to the file named by VOICE_DRIVER_LOG
// (default C:\setup\voice_driver.log) because the app has no console. The
// exit code is 0 only when every check passed.
import 'dart:async';
import 'dart:io';

import 'package:discourse_native/src/plugins/voice/voice_media.dart';
import 'package:discourse_native/src/plugins/voice/voice_models.dart';
import 'package:discourse_native/src/plugins/voice/voice_screen_share.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_webrtc/flutter_webrtc.dart' as rtc;

final String _logPath =
    Platform.environment['VOICE_DRIVER_LOG'] ?? r'C:\setup\voice_driver.log';
final List<String> _failures = [];

void _log(String message) {
  final line =
      '${DateTime.now().toIso8601String().substring(11, 23)} $message\n';
  File(_logPath).writeAsStringSync(line, mode: FileMode.append, flush: true);
}

void _check(bool ok, String what) {
  _log('${ok ? "[ok]  " : "[FAIL]"} $what');
  if (!ok) _failures.add(what);
}

/// One participant: a session plus the peer connections it created, so the
/// driver can read their statistics.
final class _Member {
  _Member(this.id, this.session, this.peers);
  final int id;
  final MeshVoiceMediaSession session;
  final List<rtc.RTCPeerConnection> peers;
  bool gone = false;
}

class _Room {
  _Room({required this.ids, this.sharedCapabilities});

  final List<int> ids;
  final Future<rtc.SharedScreenEncoderCapabilities> Function()?
  sharedCapabilities;
  final Map<int, _Member> members = {};
  // A session ignores signals from participants it has not synced yet, so
  // signals wait here until every member has the roster.
  bool _open = false;
  final List<Future<void> Function()> _held = [];

  List<VoiceParticipant> get _participants => [
    for (final id in ids)
      if (members[id]?.gone != true)
        VoiceParticipant(
          id: id,
          username: 'user$id',
          role: VoiceRole.participant,
        ),
  ];

  VoiceJoinResponse _join() => VoiceJoinResponse(
    transport: VoiceTransport.mesh,
    ice: const VoiceIceConfiguration(servers: [], relayOnly: false),
    room: VoiceRoom(
      id: 1,
      name: 'Hardware test',
      slug: 'hardware-test',
      isPublic: true,
      ephemeral: false,
      type: VoiceRoomType.open,
      videoEnabled: true,
      videoAllowed: true,
      participants: _participants,
    ),
  );

  Future<void> start() async {
    for (final id in ids) {
      final peers = <rtc.RTCPeerConnection>[];
      final session = MeshVoiceMediaSession(
        join: _join(),
        localUserId: id,
        // Only the sender publishes: a recv-only transceiver cannot send, so
        // the session would never carry the share. There is no microphone on
        // this machine, so opening one fails at once and the roster sync below
        // creates the peers anyway.
        audioPublishingAllowed: id == ids.first,
        getUserMedia: (_) async => throw StateError('no microphone here'),
        sendSignal: (recipient, event) async {
          // The session batches ICE candidates as {'events': [...]}; the
          // signaling layer delivers them one by one, so do the same.
          final batch = event['events'];
          final signals = batch is List
              ? [for (final e in batch) Map<String, dynamic>.from(e as Map)]
              : [Map<String, dynamic>.from(event)];
          Future<void> deliver() async {
            final target = members[recipient];
            if (target == null || target.gone) return;
            for (final signal in signals) {
              try {
                await target.session.handleSignal(id, signal);
              } catch (error) {
                _log(
                  'signal $id -> $recipient (${signal['type']}) threw: $error',
                );
              }
            }
          }

          for (final signal in signals) {
            if (signal['type'] != 'candidate') {
              _log('signal $id -> $recipient: ${signal['type']}');
            }
          }
          // Deliver on a later turn so a signal never re-enters its sender.
          if (_open) {
            unawaited(Future<void>.delayed(Duration.zero, deliver));
          } else {
            _held.add(deliver);
          }
        },
        createPeerConnection: (configuration) async {
          final peer = await rtc.createPeerConnection(configuration);
          peers.add(peer);
          return peer;
        },
        getSharedScreenCapabilities: id == ids.first
            ? sharedCapabilities
            : null,
      );
      members[id] = _Member(id, session, peers);
    }
    for (final member in members.values) {
      try {
        await member.session.connect();
      } catch (error) {
        _log(
          'member ${member.id} connect: $error (expected for the sender: no microphone)',
        );
      }
    }
    for (final member in members.values) {
      await member.session.syncParticipants(_participants);
    }
    _open = true;
    for (final deliver in _held) {
      unawaited(Future<void>.delayed(Duration.zero, deliver));
    }
    _held.clear();
  }

  /// Logs each peer connection's state and, for the first one, the kinds of
  /// statistics it reports, so a failure can be told from a harness problem.
  Future<void> diagnose(String label) async {
    for (final member in members.values.where((m) => !m.gone)) {
      for (var i = 0; i < member.peers.length; i++) {
        final peer = member.peers[i];
        try {
          final reports = await peer.getStats();
          final kinds = <String>{for (final r in reports) r.type};
          _log(
            '$label: member ${member.id} peer $i state=${peer.connectionState} ice=${peer.iceConnectionState} stats=${reports.length} types=${kinds.join(",")}',
          );
        } catch (error) {
          _log('$label: member ${member.id} peer $i stats error: $error');
        }
      }
    }
  }

  /// A participant leaves: the others are told, as the app does when the room
  /// roster changes, and its own session is disposed.
  Future<void> leave(int id) async {
    final member = members[id]!;
    member.gone = true;
    for (final other in members.values.where((m) => !m.gone)) {
      await other.session.syncParticipants(_participants);
    }
    await member.session.dispose();
  }

  Future<void> disposeAll() async {
    for (final member in members.values.where((m) => !m.gone)) {
      member.gone = true;
      try {
        await member.session.dispose();
      } catch (error) {
        _log('dispose of ${member.id} threw: $error');
      }
    }
  }
}

/// The first video RTP stream of the given direction among [peers].
Future<Map<String, dynamic>?> _videoStats(
  List<rtc.RTCPeerConnection> peers,
  String direction,
) async {
  for (final peer in peers) {
    List<rtc.StatsReport> reports;
    try {
      reports = await peer.getStats();
    } catch (_) {
      continue;
    }
    for (final report in reports) {
      if (report.type != '$direction-rtp') continue;
      final values = Map<String, dynamic>.from(report.values);
      final kind = '${values['kind'] ?? values['mediaType'] ?? ''}';
      if (kind == 'video') return values;
    }
  }
  return null;
}

num _num(Object? value) => value is num ? value : num.tryParse('$value') ?? 0;

Future<String> _nvencSessions() async {
  try {
    final result = await Process.run('nvidia-smi', [
      '--query-gpu=encoder.stats.sessionCount',
      '--format=csv,noheader',
    ]);
    return '${result.stdout}'.trim();
  } catch (error) {
    return 'unavailable ($error)';
  }
}

/// Frames decoded per second by [member]'s video receiver, sampled over
/// [seconds]. Also returns the last reported frame size.
Future<({double fps, int width, int height, int frames})> _decodeRate(
  _Member member,
  int seconds,
) async {
  final first = await _videoStats(member.peers, 'inbound');
  final startFrames = _num(first?['framesDecoded']).toInt();
  await Future<void>.delayed(Duration(seconds: seconds));
  final last = await _videoStats(member.peers, 'inbound');
  final endFrames = _num(last?['framesDecoded']).toInt();
  return (
    fps: (endFrames - startFrames) / seconds,
    width: _num(last?['frameWidth']).toInt(),
    height: _num(last?['frameHeight']).toInt(),
    frames: endFrames,
  );
}

Future<void> _wait(int seconds) =>
    Future<void>.delayed(Duration(seconds: seconds));

Future<void> _sharedScenario() async {
  _log('=== shared NVENC screen share, 3 viewers ===');
  final room = _Room(ids: [1, 2, 3, 4]);
  await room.start();
  final sender = room.members[1]!;
  final viewers = [room.members[2]!, room.members[3]!, room.members[4]!];
  await _wait(3);

  _check(
    await sender.session.hardwareEncodingAvailable(),
    'the sender reports the shared hardware encoder available',
  );
  sender.session.screenSharePreset = VoiceScreenSharePreset.halfSource60;
  await sender.session.setScreenShareEnabled(true);
  _check(sender.session.screenSharing, 'the sender is screen sharing');
  _check(
    sender.session.screenSharePreviewHidden(1),
    'the self-preview is replaced by the "You\'re sharing" placeholder',
  );
  await _wait(8);
  await room.diagnose('after share start');
  _check(
    sender.session.screenShareUnsupportedPeerIds.isEmpty,
    'no viewer was refused the shared stream (unsupported: ${sender.session.screenShareUnsupportedPeerIds})',
  );

  // Steady state: what each viewer decodes, measured together.
  final rates = await Future.wait(viewers.map((v) => _decodeRate(v, 10)));
  for (var i = 0; i < viewers.length; i++) {
    final r = rates[i];
    _log(
      'viewer ${viewers[i].id}: ${r.width}x${r.height}, ${r.fps.toStringAsFixed(1)} fps, ${r.frames} frames decoded',
    );
    _check(
      r.width == 3840 && r.height == 1080,
      'viewer ${viewers[i].id} decodes 3840x1080 (got ${r.width}x${r.height})',
    );
    _check(
      r.fps >= 50,
      'viewer ${viewers[i].id} sustains at least 50 fps (got ${r.fps.toStringAsFixed(1)})',
    );
  }
  final sessions = await _nvencSessions();
  _check(
    sessions == '1',
    'exactly one NVENC session serves three viewers (nvidia-smi: $sessions)',
  );
  final out = await _videoStats(sender.peers, 'outbound');
  final encoder = '${out?['encoderImplementation']}';
  _log(
    'sender outbound: encoder=$encoder ${out?['frameWidth']}x${out?['frameHeight']} limit=${out?['qualityLimitationReason']}',
  );
  _check(
    encoder.contains('SharedNVENC'),
    'the sender encodes with the shared NVENC encoder (reported "$encoder")',
  );
  _check(
    _num(out?['frameWidth']) == 3840,
    'the sender sends 3840 wide, never rescaled',
  );

  // A viewer leaves: the app's own peer-removal path.
  _log('viewer 2 leaves the room');
  final before = await Future.wait([
    _decodeRate(viewers[1], 4),
    _decodeRate(viewers[2], 4),
  ]);
  await room.leave(2);
  await _wait(2);
  final after = await Future.wait([
    _decodeRate(viewers[1], 6),
    _decodeRate(viewers[2], 6),
  ]);
  for (var i = 0; i < 2; i++) {
    _log(
      'viewer ${viewers[i + 1].id}: ${before[i].fps.toStringAsFixed(1)} fps before, ${after[i].fps.toStringAsFixed(1)} after',
    );
    _check(
      after[i].fps >= before[i].fps * 0.85,
      'viewer ${viewers[i + 1].id} is unaffected when viewer 2 leaves',
    );
  }
  _check(
    await _nvencSessions() == '1',
    'still one NVENC session after a viewer left',
  );

  // Stop and restart, three times: nothing may leak or crash.
  for (var round = 1; round <= 3; round++) {
    await sender.session.setScreenShareEnabled(false);
    await _wait(3);
    final idle = await _nvencSessions();
    _check(
      idle == '0',
      'round $round: NVENC session released when the share stops (nvidia-smi: $idle)',
    );
    _check(
      !sender.session.screenSharing,
      'round $round: the sender is no longer sharing',
    );
    await sender.session.setScreenShareEnabled(true);
    await _wait(7);
    final rate = await _decodeRate(viewers[1], 5);
    final live = await _nvencSessions();
    _log(
      'round $round: viewer ${viewers[1].id} ${rate.width}x${rate.height} ${rate.fps.toStringAsFixed(1)} fps, sessions $live',
    );
    _check(
      rate.fps >= 45 && live == '1',
      'round $round: sharing again, viewer decodes ${rate.fps.toStringAsFixed(1)} fps, one session',
    );
  }

  await sender.session.setScreenShareEnabled(false);
  await room.disposeAll();
  await _wait(3);
  final end = await _nvencSessions();
  _check(
    end == '0',
    'every NVENC session is gone after the room is torn down (nvidia-smi: $end)',
  );
}

Future<void> _fallbackScenario() async {
  _log('=== sender without the hardware encoder keeps today\'s path ===');
  final room = _Room(
    ids: [1, 2, 3],
    sharedCapabilities: () async => const rtc.SharedScreenEncoderCapabilities(
      available: false,
      reason: 'simulated: no NVENC',
    ),
  );
  await room.start();
  final sender = room.members[1]!;
  final viewers = [room.members[2]!, room.members[3]!];
  await _wait(3);
  _check(
    !await sender.session.hardwareEncodingAvailable(),
    'the sender reports no hardware encoder',
  );
  await sender.session.setScreenShareEnabled(true);
  _check(
    sender.session.screenSharing,
    'the sender is screen sharing through ordinary capture',
  );
  _check(
    !sender.session.screenSharePreviewHidden(1),
    'the self-preview is kept on the software path',
  );
  await _wait(10);
  final rates = await Future.wait(viewers.map((v) => _decodeRate(v, 8)));
  for (var i = 0; i < viewers.length; i++) {
    final r = rates[i];
    _log(
      'viewer ${viewers[i].id}: ${r.width}x${r.height}, ${r.fps.toStringAsFixed(1)} fps',
    );
    _check(
      r.frames > 30 && r.fps >= 5,
      'viewer ${viewers[i].id} receives the software-encoded share (${r.fps.toStringAsFixed(1)} fps)',
    );
  }
  final out = await _videoStats(sender.peers, 'outbound');
  _log(
    'sender outbound: encoder=${out?['encoderImplementation']} ${out?['frameWidth']}x${out?['frameHeight']}',
  );
  _check(
    !'${out?['encoderImplementation']}'.contains('SharedNVENC'),
    'the software path does not use the shared encoder',
  );
  final sessions = await _nvencSessions();
  _check(
    sessions == '0',
    'no NVENC session is open on the software path (nvidia-smi: $sessions)',
  );
  await sender.session.setScreenShareEnabled(false);
  await room.disposeAll();
  _log('software path torn down without a crash');
}

/// The first virtual display's GDI name, from display_tool.
Future<String?> _virtualDisplay(String tools) async {
  final result = await Process.run('$tools\\display_tool.exe', ['list']);
  for (final line in '${result.stdout}'.split('\n')) {
    if (line.contains('Virtual Display Driver')) {
      return line.split('|').first.trim();
    }
  }
  return null;
}

/// A display change under a live share killed the share for good (the capture
/// fails permanently, the encoder is released, viewers freeze, the session
/// still reports sharing). With the watchdog the share must come back at the
/// new size. Run on a build that has it.
Future<void> _watchdogScenario() async {
  _log('=== display change under a live share: the share must come back');
  final tools =
      Platform.environment['VOICE_TOOLS'] ?? r'C:\w\build\nvenc_smoke_test';
  final display = await _virtualDisplay(tools);
  if (display == null) {
    _check(false, 'a virtual display exists');
    return;
  }
  final room = _Room(ids: [1, 2, 3]);
  await room.start();
  final sender = room.members[1]!;
  final viewer = room.members[2]!;
  await _wait(3);
  sender.session.screenSharePreset = VoiceScreenSharePreset.halfSource60;
  await sender.session.setScreenShareEnabled(true);
  await _wait(10);
  final before = await _decodeRate(viewer, 5);
  _log(
    'before: ${before.width}x${before.height}, ${before.fps.toStringAsFixed(1)} fps',
  );
  _check(
    before.width == 3840 && before.fps >= 45,
    'the share is healthy before the change',
  );

  _log('changing $display to 3840x2160');
  await Process.run('$tools\\display_tool.exe', [
    'set',
    display,
    '3840',
    '2160',
    '60',
  ]);
  final started = DateTime.now();
  var recovered = false;
  var lastFrames = 0;
  for (var i = 0; i < 25 && !recovered; i++) {
    await _wait(2);
    final stats = await _videoStats(viewer.peers, 'inbound');
    final frames = _num(stats?['framesDecoded']).toInt();
    final width = _num(stats?['frameWidth']).toInt();
    final height = _num(stats?['frameHeight']).toInt();
    final moving = frames - lastFrames;
    lastFrames = frames;
    final seconds = DateTime.now().difference(started).inSeconds;
    _log(
      '  +${seconds}s: ${width}x$height, $moving frames in the last 2 s, sessions ${await _nvencSessions()}',
    );
    if (width == 1920 && height == 1080 && moving >= 60) recovered = true;
  }
  _check(
    recovered,
    'viewers get video again at the new size (1920x1080) within 50 s',
  );
  _check(sender.session.screenSharing, 'the session is still sharing');
  _check(await _nvencSessions() == '1', 'one NVENC session after the restart');

  await Process.run('$tools\\display_tool.exe', [
    'set',
    display,
    '7680',
    '2160',
    '60',
  ]);
  await sender.session.setScreenShareEnabled(false);
  await room.disposeAll();
}

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  try {
    File(_logPath).writeAsStringSync('');
  } catch (_) {}
  _log(
    'voice hardware driver, pid $pid, session ${Platform.environment['SESSIONNAME']}',
  );
  var code = 0;
  try {
    final which = Platform.environment['VOICE_DRIVER_SCENARIO'] ?? 'all';
    if (which == 'watchdog') {
      await _watchdogScenario().timeout(const Duration(minutes: 5));
    } else {
      await _sharedScenario().timeout(const Duration(minutes: 6));
      await _fallbackScenario().timeout(const Duration(minutes: 4));
    }
  } catch (error, stack) {
    _failures.add('exception: $error');
    _log('EXCEPTION: $error\n$stack');
  }
  if (_failures.isEmpty) {
    _log('ALL CHECKS PASSED');
  } else {
    code = 1;
    _log('FAILED CHECKS:');
    for (final failure in _failures) {
      _log('  - $failure');
    }
  }
  _log('exiting with $code');
  exit(code);
}
