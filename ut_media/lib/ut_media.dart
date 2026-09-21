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

  static Future<void> haptic() => _invoke<void>('haptic');

  static Future<void> startRecording(String path) =>
      _channel.invokeMethod<void>('startRecording', path);

  static Future<String?> stopRecording() =>
      _channel.invokeMethod<String>('stopRecording');

  static Future<void> cancelRecording() => _invoke<void>('cancelRecording');

  static Future<void> pauseRecording() => _invoke<void>('pauseRecording');

  static Future<void> resumeRecording() => _invoke<void>('resumeRecording');

  static Future<double> amplitude() async {
    return await _invoke<double>('amplitude') ?? -60;
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
