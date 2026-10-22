import 'package:flutter/services.dart';

/// PulseAudio voice notes, Xylo chime, and haptic on Ubuntu Touch.
///
/// Capture matches Cinny’s native QAudioInput path: PCM from the phone’s
/// default microphone at the source’s own rate, then 16 kHz SILK Opus.
class UtMedia {
  UtMedia._();

  static const _channel = MethodChannel('ut_media');

  static Future<void> playAlert() => _invoke<void>('playAlert');

  static Future<void> playFile(String path) =>
      _channel.invokeMethod<void>('playFile', path);

  static Future<void> stopPlayback() => _invoke<void>('stopPlayback');

  static Future<void> pausePlayback() => _invoke<void>('pausePlayback');

  static Future<void> resumePlayback() => _invoke<void>('resumePlayback');

  /// Live playhead from the native Pulse/GStreamer player.
  ///
  /// `{positionMs, durationMs, playing}` — `playing` is 1 while the native
  /// play thread is still active (including pause).
  static Future<Map<String, int>> playbackPosition() async {
    try {
      final raw =
          await _channel.invokeMethod<Map<dynamic, dynamic>>('playbackPosition');
      if (raw == null) {
        return const {'positionMs': 0, 'durationMs': 0, 'playing': 0};
      }
      int asInt(Object? v) {
        if (v is int) return v;
        if (v is num) return v.toInt();
        return 0;
      }

      return {
        'positionMs': asInt(raw['positionMs']),
        'durationMs': asInt(raw['durationMs']),
        'playing': asInt(raw['playing']),
      };
    } on MissingPluginException {
      return const {'positionMs': 0, 'durationMs': 0, 'playing': 0};
    } on PlatformException catch (e) {
      throw Exception(e.message ?? e.code);
    }
  }

  static Future<void> seekPlayback(int positionMs) =>
      _invoke<void>('seekPlayback', positionMs);

  static Future<void> haptic() => _invoke<void>('haptic');

  /// Must match `kMaxSeconds` in `ut_media_plugin.cc`.
  static const int maxRecordingSeconds = 600;

  static Future<void> startRecording(String path) =>
      _channel.invokeMethod<void>('startRecording', path);

  /// Stops capture and returns `{path, durationMs}` for the encoded file.
  static Future<Map<String, Object?>?> stopRecording() async {
    try {
      final raw =
          await _channel.invokeMethod<Map<dynamic, dynamic>>('stopRecording');
      if (raw == null) return null;
      return Map<String, Object?>.from(raw);
    } on MissingPluginException {
      return null;
    } on PlatformException catch (e) {
      throw Exception(e.message ?? e.code);
    }
  }

  static Future<void> cancelRecording() => _invoke<void>('cancelRecording');

  static Future<void> pauseRecording() => _invoke<void>('pauseRecording');

  static Future<void> resumeRecording() => _invoke<void>('resumeRecording');

  static Future<double> amplitude() async {
    return await _invoke<double>('amplitude') ?? -60;
  }

  /// Address-book rows from `com.lomiri.pim`: `{name, phones: [String]}`.
  static Future<List<Map<String, Object?>>> listContacts() async {
    final raw = await _channel.invokeMethod<List<dynamic>>('listContacts');
    if (raw == null) return const [];
    return raw
        .whereType<Map>()
        .map((row) => Map<String, Object?>.from(row))
        .toList();
  }

  static Future<T?> _invoke<T>(String method, [Object? args]) async {
    try {
      return await _channel.invokeMethod<T>(method, args);
    } on MissingPluginException {
      return null;
    } on PlatformException catch (e) {
      throw Exception(e.message ?? e.code);
    }
  }
}
