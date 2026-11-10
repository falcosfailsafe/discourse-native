// Software-versus-NVENC baseline for Voice screen share on Windows.
//
// Phase 0 of docs/plans/voice-hardware-encoding.md asks for the CPU and frame
// rate of today's software screen share at 1080p and at the 7680x2160 source.
// This driver measures that, and the shared NVENC path at the same sizes, so
// the two can be compared.
//
// The sender and the viewers run in TWO processes so the sender's CPU is not
// inflated by the viewers' software decoders: the same executable is started
// twice, as VOICE_ROLE=viewers first and VOICE_ROLE=sender second, joined by a
// localhost signaling bridge (VOICE_PORT). Each case is repeated VOICE_RUNS
// times (default 3). Results go to VOICE_DRIVER_LOG.
//
//   flutter build windows --release --target integration_test/voice_windows_baseline_driver.dart
//
// Needs the machine described in tool/libwebrtc/native_tests/windows/README.md:
// a display in the interactive session, display_tool.exe and noise_window.exe.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:discourse_native/src/plugins/voice/voice_media.dart';
import 'package:discourse_native/src/plugins/voice/voice_models.dart';
import 'package:discourse_native/src/plugins/voice/voice_screen_share.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_webrtc/flutter_webrtc.dart' as rtc;

final String _role = Platform.environment['VOICE_ROLE'] ?? 'sender';
final int _port = int.parse(Platform.environment['VOICE_PORT'] ?? '47123');
final int _runs = int.parse(Platform.environment['VOICE_RUNS'] ?? '3');
// VOICE_MODES=software,nvenc (default both). The DLL probes the encoder once per
// process and caches the answer, so each mode is best run in its own process.
final Set<String> _modes =
    (Platform.environment['VOICE_MODES'] ?? 'software,nvenc')
        .split(',')
        .toSet();
final String _tools =
    Platform.environment['VOICE_TOOLS'] ?? r'C:\w\build\nvenc_smoke_test';
final String _logPath =
    Platform.environment['VOICE_DRIVER_LOG'] ??
    (_role == 'sender'
        ? r'C:\setup\baseline.log'
        : r'C:\setup\baseline_viewers.log');

void _log(String message) {
  final line =
      '${DateTime.now().toIso8601String().substring(11, 23)} $message\n';
  File(_logPath).writeAsStringSync(line, mode: FileMode.append, flush: true);
}

num _num(Object? value) => value is num ? value : num.tryParse('$value') ?? 0;

/// Whole-process CPU time in seconds, read through PowerShell.
Future<double> _cpuSeconds() async {
  final result = await Process.run('powershell', [
    '-NoProfile',
    '-Command',
    '(Get-Process -Id $pid).TotalProcessorTime.TotalSeconds',
  ]);
  return double.tryParse('${result.stdout}'.trim()) ?? 0;
}

Future<String> _gpu() async {
  try {
    final r = await Process.run('nvidia-smi', [
      '--query-gpu=utilization.gpu,utilization.encoder,utilization.decoder,encoder.stats.sessionCount',
      '--format=csv,noheader,nounits',
    ]);
    return '${r.stdout}'.trim();
  } catch (_) {
    return '?';
  }
}

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
      if ('${values['kind'] ?? values['mediaType']}' == 'video') return values;
    }
  }
  return null;
}

/// Line-delimited JSON over one TCP socket.
final class _Link {
  _Link(this._socket) {
    _socket
        .cast<List<int>>()
        .transform(utf8.decoder)
        .transform(const LineSplitter())
        .listen((line) {
          if (line.isEmpty) return;
          _messages.add(jsonDecode(line) as Map<String, dynamic>);
        }, onDone: () => _messages.close());
  }
  final Socket _socket;
  final StreamController<Map<String, dynamic>> _messages =
      StreamController.broadcast();
  Stream<Map<String, dynamic>> get messages => _messages.stream;
  void send(Map<String, dynamic> message) =>
      _socket.write('${jsonEncode(message)}\n');
  Future<Map<String, dynamic>> next(
    bool Function(Map<String, dynamic>) test, {
    Duration timeout = const Duration(seconds: 90),
  }) => messages.firstWhere(test).timeout(timeout);
}

/// A set of sessions in this process, wired to each other directly and to the
/// other process through [link].
final class _Local {
  _Local(this.ids, this.allIds, this.link, {this.unavailable = false});
  final List<int> ids;
  final List<int> allIds;
  final _Link link;
  final bool unavailable;
  final Map<int, MeshVoiceMediaSession> sessions = {};
  final Map<int, List<rtc.RTCPeerConnection>> peers = {};
  bool _open = false;
  final List<Future<void> Function()> _held = [];
  StreamSubscription<Map<String, dynamic>>? _inbound;

  VoiceJoinResponse _join() => VoiceJoinResponse(
    transport: VoiceTransport.mesh,
    ice: const VoiceIceConfiguration(servers: [], relayOnly: false),
    room: VoiceRoom(
      id: 1,
      name: 'Baseline',
      slug: 'baseline',
      isPublic: true,
      ephemeral: false,
      type: VoiceRoomType.open,
      videoEnabled: true,
      videoAllowed: true,
      participants: [
        for (final id in allIds)
          VoiceParticipant(
            id: id,
            username: 'user$id',
            role: VoiceRole.participant,
          ),
      ],
    ),
  );

  Future<void> start({required bool publisher}) async {
    _inbound = link.messages.listen((m) {
      if (m['signal'] is! Map) return;
      final to = m['to'] as int;
      final from = m['from'] as int;
      final signal = Map<String, dynamic>.from(m['signal'] as Map);
      unawaited(
        sessions[to]?.handleSignal(from, signal) ?? Future<void>.value(),
      );
    });
    for (final id in ids) {
      final list = <rtc.RTCPeerConnection>[];
      peers[id] = list;
      sessions[id] = MeshVoiceMediaSession(
        join: _join(),
        localUserId: id,
        audioPublishingAllowed: publisher && id == ids.first,
        getUserMedia: (_) async => throw StateError('no microphone here'),
        getSharedScreenCapabilities: unavailable && publisher
            ? () async => const rtc.SharedScreenEncoderCapabilities(
                available: false,
                reason: 'forced software path',
              )
            : null,
        createPeerConnection: (configuration) async {
          final peer = await rtc.createPeerConnection(configuration);
          list.add(peer);
          return peer;
        },
        sendSignal: (recipient, event) async {
          final batch = event['events'];
          final signals = batch is List
              ? [for (final e in batch) Map<String, dynamic>.from(e as Map)]
              : [Map<String, dynamic>.from(event)];
          Future<void> deliver() async {
            for (final signal in signals) {
              if (ids.contains(recipient)) {
                await sessions[recipient]?.handleSignal(id, signal);
              } else {
                link.send({'from': id, 'to': recipient, 'signal': signal});
              }
            }
          }

          if (_open) {
            unawaited(Future<void>.delayed(Duration.zero, deliver));
          } else {
            _held.add(deliver);
          }
        },
      );
    }
    for (final session in sessions.values) {
      try {
        await session.connect();
      } catch (_) {}
    }
    final roster = [
      for (final id in allIds)
        VoiceParticipant(
          id: id,
          username: 'user$id',
          role: VoiceRole.participant,
        ),
    ];
    for (final session in sessions.values) {
      await session.syncParticipants(roster);
    }
    _open = true;
    for (final deliver in _held) {
      unawaited(Future<void>.delayed(Duration.zero, deliver));
    }
    _held.clear();
  }

  Future<void> dispose() async {
    await _inbound?.cancel();
    for (final session in sessions.values) {
      try {
        await session.dispose();
      } catch (error) {
        _log('dispose threw: $error');
      }
    }
  }
}

// ----------------------------------------------------------------- viewers

Future<void> _viewers() async {
  _log('viewers process, pid $pid');
  final server = await ServerSocket.bind(InternetAddress.loopbackIPv4, _port);
  final link = _Link(await server.first.timeout(const Duration(minutes: 3)));
  _log('sender connected');
  _Local? local;
  await for (final m in link.messages) {
    switch (m['cmd']) {
      case 'room':
        await local?.dispose();
        local = _Local([2, 3, 4], [1, 2, 3, 4], link);
        await local.start(publisher: false);
        link.send({'reply': 'room'});
      case 'measure':
        final seconds = m['seconds'] as int;
        final active = local!;
        final startFrames = <int, int>{};
        for (final id in active.ids) {
          final s = await _videoStats(active.peers[id]!, 'inbound');
          startFrames[id] = _num(s?['framesDecoded']).toInt();
        }
        final cpu0 = await _cpuSeconds();
        final t0 = DateTime.now();
        await Future<void>.delayed(Duration(seconds: seconds));
        final cpu1 = await _cpuSeconds();
        final wall = DateTime.now().difference(t0).inMilliseconds / 1000.0;
        final fps = <double>[];
        var width = 0, height = 0;
        for (final id in active.ids) {
          final s = await _videoStats(active.peers[id]!, 'inbound');
          fps.add(
            (_num(s?['framesDecoded']).toInt() - startFrames[id]!) / wall,
          );
          width = _num(s?['frameWidth']).toInt();
          height = _num(s?['frameHeight']).toInt();
        }
        link.send({
          'reply': 'measure',
          'fps': fps,
          'width': width,
          'height': height,
          'cpuPercent':
              (cpu1 - cpu0) / wall / Platform.numberOfProcessors * 100,
          'cpuCores': (cpu1 - cpu0) / wall,
        });
      case 'teardown':
        await local?.dispose();
        local = null;
        link.send({'reply': 'teardown'});
      case 'quit':
        _log('quit');
        exit(0);
    }
  }
}

// ------------------------------------------------------------------ sender

Future<void> _setDisplay(int w, int h) async {
  await Process.run('$_tools\\display_tool.exe', [
    'set',
    r'\\.\DISPLAY6',
    '$w',
    '$h',
    '60',
  ]);
  await Future<void>.delayed(const Duration(seconds: 3));
}

Future<Process> _noise(int w, int h) => Process.start(
  '$_tools\\noise_window.exe',
  ['0', '0', '$w', '$h', '300', '8', '0.2'],
);

Future<void> _sender() async {
  _log('sender process, pid $pid, ${Platform.numberOfProcessors} logical CPUs');
  // The link lives until the process exits.
  // ignore: close_sinks
  Socket? socket;
  for (var i = 0; i < 120 && socket == null; i++) {
    try {
      socket = await Socket.connect(InternetAddress.loopbackIPv4, _port);
    } catch (_) {
      await Future<void>.delayed(const Duration(seconds: 1));
    }
  }
  if (socket == null) throw StateError('the viewers process never listened');
  final link = _Link(socket);

  final cases = <({int w, int h, VoiceScreenSharePreset preset})>[
    (w: 1920, h: 1080, preset: VoiceScreenSharePreset.p1080x60),
    (w: 7680, h: 2160, preset: VoiceScreenSharePreset.halfSource60),
  ];
  for (final c in cases) {
    for (final nvenc in [false, true]) {
      if (!_modes.contains(nvenc ? 'nvenc' : 'software')) continue;
      for (var run = 1; run <= _runs; run++) {
        final label =
            '${nvenc ? "nvenc   " : "software"} display ${c.w}x${c.h} run $run';
        _log('--- $label');
        await _setDisplay(c.w, c.h);
        final noise = await _noise(c.w, c.h);
        await Future<void>.delayed(const Duration(seconds: 3));
        link.send({'cmd': 'room'});
        await link.next((m) => m['reply'] == 'room');
        final local = _Local([1], [1, 2, 3, 4], link, unavailable: !nvenc);
        await local.start(publisher: true);
        final session = local.sessions[1]!;
        session.screenSharePreset = c.preset;
        await Future<void>.delayed(const Duration(seconds: 3));
        final hardware = await session.hardwareEncodingAvailable();
        await session.setScreenShareEnabled(true);
        await Future<void>.delayed(const Duration(seconds: 10));

        link.send({'cmd': 'measure', 'seconds': 20});
        final cpu0 = await _cpuSeconds();
        final out0 = await _videoStats(local.peers[1]!, 'outbound');
        final t0 = DateTime.now();
        final viewers = await link.next(
          (m) => m['reply'] == 'measure',
          timeout: const Duration(seconds: 120),
        );
        final cpu1 = await _cpuSeconds();
        final out1 = await _videoStats(local.peers[1]!, 'outbound');
        final wall = DateTime.now().difference(t0).inMilliseconds / 1000.0;
        final gpu = await _gpu();
        final sent =
            (_num(out1?['framesEncoded']) - _num(out0?['framesEncoded'])) /
            wall;
        final kbps =
            (_num(out1?['bytesSent']) - _num(out0?['bytesSent'])) *
            8 /
            wall /
            1000;
        final fps = [
          for (final v in viewers['fps'] as List) (v as num).toDouble(),
        ];
        _log(
          'RESULT ${nvenc ? "nvenc" : "software"} display=${c.w}x${c.h} run=$run hardware=$hardware '
          'sender_cpu=${((cpu1 - cpu0) / wall / Platform.numberOfProcessors * 100).toStringAsFixed(1)}% '
          '(${((cpu1 - cpu0) / wall).toStringAsFixed(2)} cores) '
          'sender_encoded=${sent.toStringAsFixed(1)}fps '
          'sent=${(kbps / 1000).toStringAsFixed(1)}Mbps(all) '
          'frame=${out1?['frameWidth']}x${out1?['frameHeight']} encoder=${out1?['encoderImplementation']} '
          'limit=${out1?['qualityLimitationReason']} '
          'viewers=${viewers['width']}x${viewers['height']} '
          'decoded=${fps.map((f) => f.toStringAsFixed(1)).join("/")}fps '
          'viewers_cpu=${(viewers['cpuPercent'] as num).toStringAsFixed(1)}% '
          '(${(viewers['cpuCores'] as num).toStringAsFixed(2)} cores) '
          'gpu,enc,dec,sessions=$gpu',
        );
        await session.setScreenShareEnabled(false);
        await local.dispose();
        link.send({'cmd': 'teardown'});
        await link.next((m) => m['reply'] == 'teardown');
        noise.kill();
        await Future<void>.delayed(const Duration(seconds: 3));
      }
    }
  }
  link.send({'cmd': 'quit'});
  _log('BASELINE DONE');
  await Future<void>.delayed(const Duration(seconds: 1));
}

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  try {
    File(_logPath).writeAsStringSync('');
  } catch (_) {}
  try {
    if (_role == 'viewers') {
      await _viewers();
    } else {
      await _sender();
    }
  } catch (error, stack) {
    _log('EXCEPTION: $error\n$stack');
    exit(1);
  }
  exit(0);
}
