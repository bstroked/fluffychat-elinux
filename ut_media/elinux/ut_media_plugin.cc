#include "include/ut_media/ut_media_plugin.h"

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>
#include <flutter/standard_method_codec.h>
#include <gio/gio.h>
#include <opus/opus.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

#ifdef UT_MEDIA_HAVE_VORBIS
#include <vorbis/vorbisfile.h>
#endif
#ifdef UT_MEDIA_HAVE_OPUSFILE
#include <opusfile.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ut_media {
namespace {

constexpr char kChannelName[] = "ut_media";
constexpr int kEncoderRate = 16000;
constexpr int kFrameMs = 20;
constexpr int kFrameSamples = kEncoderRate * kFrameMs / 1000;
constexpr int kBitrate = 20000;
constexpr int kPreSkip = 312;
constexpr int kGranulePerFrame = 48 * kFrameMs;
constexpr int kFramesPerPacket = 6;
constexpr int kPacketsPerPage = 16;
constexpr int kMaxSeconds = 120;
constexpr char kXyloPath[] =
    "/usr/share/sounds/lomiri/notifications/Xylo.ogg";

void WriteU16(std::vector<uint8_t>* o, uint16_t v) {
  o->push_back(static_cast<uint8_t>(v & 0xff));
  o->push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

void WriteU32(std::vector<uint8_t>* o, uint32_t v) {
  o->push_back(static_cast<uint8_t>(v & 0xff));
  o->push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  o->push_back(static_cast<uint8_t>((v >> 16) & 0xff));
  o->push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}

uint32_t OggCrc(const uint8_t* data, int len) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 256; ++i) {
      uint32_t r = static_cast<uint32_t>(i) << 24;
      for (int j = 0; j < 8; ++j) {
        r = (r & 0x80000000u) ? ((r << 1) ^ 0x04c11db7u) : (r << 1);
      }
      table[i] = r;
    }
    init = true;
  }
  uint32_t crc = 0;
  for (int i = 0; i < len; ++i) {
    crc = ((crc << 8) ^ table[((crc >> 24) ^ data[i]) & 0xff]) & 0xffffffffu;
  }
  return crc;
}

std::vector<uint8_t> EncodeOpusLen(int n) {
  std::vector<uint8_t> out;
  if (n < 252) {
    out.push_back(static_cast<uint8_t>(n));
  } else {
    out.push_back(static_cast<uint8_t>(252 + (n & 3)));
    out.push_back(static_cast<uint8_t>(n >> 2));
  }
  return out;
}

std::vector<uint8_t> MuxGroup(const std::vector<std::vector<uint8_t>>& group) {
  if (group.empty()) return {};
  const uint8_t toc = group[0][0];
  std::vector<std::vector<uint8_t>> payloads;
  payloads.reserve(group.size());
  for (const auto& f : group) {
    payloads.emplace_back(f.begin() + 1, f.end());
  }
  std::vector<uint8_t> out;
  if (payloads.size() == 1) {
    out.push_back(static_cast<uint8_t>(toc & 0xfc));
    out.insert(out.end(), payloads[0].begin(), payloads[0].end());
    return out;
  }
  if (payloads.size() == 2) {
    out.push_back(static_cast<uint8_t>((toc & 0xfc) | 2));
    auto len = EncodeOpusLen(static_cast<int>(payloads[0].size()));
    out.insert(out.end(), len.begin(), len.end());
    out.insert(out.end(), payloads[0].begin(), payloads[0].end());
    out.insert(out.end(), payloads[1].begin(), payloads[1].end());
    return out;
  }
  out.push_back(static_cast<uint8_t>((toc & 0xfc) | 3));
  out.push_back(static_cast<uint8_t>(0x80 | payloads.size()));
  for (size_t i = 0; i + 1 < payloads.size(); ++i) {
    auto len = EncodeOpusLen(static_cast<int>(payloads[i].size()));
    out.insert(out.end(), len.begin(), len.end());
  }
  for (const auto& p : payloads) {
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

std::vector<uint8_t> WriteOggPage(uint32_t serial, uint32_t seq, uint64_t granule,
                                 uint8_t header_type,
                                 const std::vector<std::vector<uint8_t>>& packets) {
  std::vector<uint8_t> segs;
  std::vector<uint8_t> body;
  for (const auto& pkt : packets) {
    int remaining = static_cast<int>(pkt.size());
    int off = 0;
    while (true) {
      const int chunk = std::min(255, remaining);
      segs.push_back(static_cast<uint8_t>(chunk));
      body.insert(body.end(), pkt.begin() + off, pkt.begin() + off + chunk);
      remaining -= chunk;
      off += chunk;
      if (chunk < 255) break;
      if (remaining == 0) {
        segs.push_back(0);
        break;
      }
    }
  }
  std::vector<uint8_t> page(27 + segs.size() + body.size());
  page[0] = 'O';
  page[1] = 'g';
  page[2] = 'g';
  page[3] = 'S';
  page[4] = 0;
  page[5] = header_type;
  page[6] = static_cast<uint8_t>(granule & 0xff);
  page[7] = static_cast<uint8_t>((granule >> 8) & 0xff);
  page[8] = static_cast<uint8_t>((granule >> 16) & 0xff);
  page[9] = static_cast<uint8_t>((granule >> 24) & 0xff);
  page[10] = static_cast<uint8_t>((granule >> 32) & 0xff);
  page[11] = static_cast<uint8_t>((granule >> 40) & 0xff);
  page[12] = static_cast<uint8_t>((granule >> 48) & 0xff);
  page[13] = static_cast<uint8_t>((granule >> 56) & 0xff);
  page[14] = static_cast<uint8_t>(serial & 0xff);
  page[15] = static_cast<uint8_t>((serial >> 8) & 0xff);
  page[16] = static_cast<uint8_t>((serial >> 16) & 0xff);
  page[17] = static_cast<uint8_t>((serial >> 24) & 0xff);
  page[18] = static_cast<uint8_t>(seq & 0xff);
  page[19] = static_cast<uint8_t>((seq >> 8) & 0xff);
  page[20] = static_cast<uint8_t>((seq >> 16) & 0xff);
  page[21] = static_cast<uint8_t>((seq >> 24) & 0xff);
  page[22] = page[23] = page[24] = page[25] = 0;
  page[26] = static_cast<uint8_t>(segs.size());
  std::memcpy(page.data() + 27, segs.data(), segs.size());
  std::memcpy(page.data() + 27 + segs.size(), body.data(), body.size());
  const uint32_t crc = OggCrc(page.data(), static_cast<int>(page.size()));
  page[22] = static_cast<uint8_t>(crc & 0xff);
  page[23] = static_cast<uint8_t>((crc >> 8) & 0xff);
  page[24] = static_cast<uint8_t>((crc >> 16) & 0xff);
  page[25] = static_cast<uint8_t>((crc >> 24) & 0xff);
  return page;
}

std::vector<uint8_t> MakeOpusHeadPacket() {
  std::vector<uint8_t> h;
  h.insert(h.end(), {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'});
  h.push_back(1);
  h.push_back(1);
  WriteU16(&h, kPreSkip);
  WriteU32(&h, kEncoderRate);
  h.push_back(0);
  h.push_back(0);
  h.push_back(0);
  return h;
}

std::vector<uint8_t> MakeOpusTagsPacket() {
  const char* vendor = "CinnyUT";
  const char* comment = "ENCODER=cinny-pulse-opus";
  std::vector<uint8_t> t;
  t.insert(t.end(), {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'});
  WriteU32(&t, static_cast<uint32_t>(std::strlen(vendor)));
  t.insert(t.end(), vendor, vendor + std::strlen(vendor));
  WriteU32(&t, 1);
  WriteU32(&t, static_cast<uint32_t>(std::strlen(comment)));
  t.insert(t.end(), comment, comment + std::strlen(comment));
  return t;
}

std::vector<uint8_t> BuildOgg(const std::vector<std::vector<uint8_t>>& frames) {
  const uint32_t serial = static_cast<uint32_t>(
      std::chrono::steady_clock::now().time_since_epoch().count() & 0xffffffffu);
  std::vector<std::vector<uint8_t>> muxed;
  std::vector<int> mux_frames;
  for (size_t i = 0; i < frames.size(); i += kFramesPerPacket) {
    std::vector<std::vector<uint8_t>> group;
    const size_t n = std::min(static_cast<size_t>(kFramesPerPacket),
                              frames.size() - i);
    for (size_t j = 0; j < n; ++j) group.push_back(frames[i + j]);
    muxed.push_back(MuxGroup(group));
    mux_frames.push_back(static_cast<int>(n));
  }
  std::vector<uint8_t> out;
  auto head = WriteOggPage(serial, 0, 0, 2, {MakeOpusHeadPacket()});
  out.insert(out.end(), head.begin(), head.end());
  auto tags = WriteOggPage(serial, 1, 0, 0, {MakeOpusTagsPacket()});
  out.insert(out.end(), tags.begin(), tags.end());
  uint32_t seq = 2;
  int frames_done = 0;
  for (size_t i = 0; i < muxed.size(); i += kPacketsPerPage) {
    std::vector<std::vector<uint8_t>> chunk;
    const size_t n =
        std::min(static_cast<size_t>(kPacketsPerPage), muxed.size() - i);
    for (size_t j = 0; j < n; ++j) {
      chunk.push_back(muxed[i + j]);
      frames_done += mux_frames[i + j];
    }
    const uint64_t granule = static_cast<uint64_t>(kPreSkip) +
                             static_cast<uint64_t>(frames_done) * kGranulePerFrame;
    auto page = WriteOggPage(serial, seq, granule, 0, chunk);
    out.insert(out.end(), page.begin(), page.end());
    seq += 1;
  }
  return out;
}

std::vector<int16_t> ToMono16(const std::vector<int16_t>& interleaved, int ch) {
  if (ch <= 1) return interleaved;
  const int frames = static_cast<int>(interleaved.size()) / ch;
  std::vector<int16_t> out(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    int sum = 0;
    for (int c = 0; c < ch; ++c) {
      sum += interleaved[static_cast<size_t>(i * ch + c)];
    }
    out[static_cast<size_t>(i)] = static_cast<int16_t>(sum / ch);
  }
  return out;
}

std::vector<int16_t> ResampleTo16k(const std::vector<int16_t>& in, int in_rate) {
  if (in_rate == kEncoderRate || in.empty()) return in;
  if (in_rate == 48000) {
    const int out_n = static_cast<int>(in.size()) / 3;
    std::vector<int16_t> out(static_cast<size_t>(out_n));
    for (int i = 0; i < out_n; ++i) {
      const int s = static_cast<int>(in[static_cast<size_t>(i * 3)]) +
                    in[static_cast<size_t>(i * 3 + 1)] +
                    in[static_cast<size_t>(i * 3 + 2)];
      out[static_cast<size_t>(i)] = static_cast<int16_t>(s / 3);
    }
    return out;
  }
  const int out_n = static_cast<int>(
      (static_cast<int64_t>(in.size()) * kEncoderRate) / in_rate);
  std::vector<int16_t> out(static_cast<size_t>(std::max(0, out_n)));
  for (int i = 0; i < out_n; ++i) {
    const double src = (static_cast<double>(i) * in_rate) / kEncoderRate;
    int i0 = static_cast<int>(src);
    int i1 = i0 + 1;
    if (i0 >= static_cast<int>(in.size())) i0 = static_cast<int>(in.size()) - 1;
    if (i1 >= static_cast<int>(in.size())) i1 = static_cast<int>(in.size()) - 1;
    const double t = src - i0;
    out[static_cast<size_t>(i)] = static_cast<int16_t>(
        in[static_cast<size_t>(i0)] * (1.0 - t) +
        in[static_cast<size_t>(i1)] * t);
  }
  return out;
}

bool EncodeOpusFile(const std::vector<int16_t>& pcm, const std::string& path) {
  if (pcm.size() < static_cast<size_t>(kFrameSamples / 2)) return false;
  int err = OPUS_OK;
  OpusEncoder* enc =
      opus_encoder_create(kEncoderRate, 1, OPUS_APPLICATION_VOIP, &err);
  if (!enc || err != OPUS_OK) {
    if (enc) opus_encoder_destroy(enc);
    return false;
  }
  opus_encoder_ctl(enc, OPUS_SET_BITRATE(kBitrate));
  opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10));
  opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  opus_encoder_ctl(enc, OPUS_SET_DTX(0));

  std::vector<std::vector<uint8_t>> frames;
  unsigned char packet[4000];
  size_t offset = 0;
  while (offset < pcm.size()) {
    int16_t frame[kFrameSamples];
    std::memset(frame, 0, sizeof(frame));
    const size_t n = std::min(static_cast<size_t>(kFrameSamples), pcm.size() - offset);
    std::memcpy(frame, pcm.data() + offset, n * sizeof(int16_t));
    offset += n;
    const int len = opus_encode(enc, frame, kFrameSamples, packet, sizeof(packet));
    if (len < 1) {
      opus_encoder_destroy(enc);
      return false;
    }
    frames.emplace_back(packet, packet + len);
  }
  opus_encoder_destroy(enc);
  const auto ogg = BuildOgg(frames);
  if (ogg.size() < 64) return false;
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(ogg.data()),
            static_cast<std::streamsize>(ogg.size()));
  return static_cast<bool>(out);
}

std::string WritableDir() {
  const char* cache = std::getenv("XDG_CACHE_HOME");
  if (cache && cache[0]) return cache;
  const char* data = std::getenv("XDG_DATA_HOME");
  if (data && data[0]) return data;
  const char* home = std::getenv("HOME");
  if (home && home[0]) {
    return std::string(home) + "/.local/share/fluffychat.notkit";
  }
  return "/tmp";
}

struct SourceQuery {
  pa_sample_spec spec{};
  std::string name;
  bool have_name = false;
  bool have_spec = false;
};

void ServerInfoCb(pa_context* ctx, const pa_server_info* info, void* userdata) {
  auto* q = static_cast<SourceQuery*>(userdata);
  if (!info || !info->default_source_name) return;
  q->name = info->default_source_name;
  q->have_name = true;
  pa_operation_unref(pa_context_get_source_info_by_name(
      ctx, info->default_source_name,
      [](pa_context*, const pa_source_info* src, int eol, void* ud) {
        if (eol > 0) return;
        auto* query = static_cast<SourceQuery*>(ud);
        if (!src) return;
        query->spec = src->sample_spec;
        query->have_spec = true;
      },
      q));
}

bool QueryDefaultSource(pa_sample_spec* spec_out) {
  pa_mainloop* ml = pa_mainloop_new();
  if (!ml) return false;
  pa_context* ctx =
      pa_context_new(pa_mainloop_get_api(ml), "fluffychat-source-query");
  if (!ctx) {
    pa_mainloop_free(ml);
    return false;
  }
  SourceQuery query;
  pa_context_set_state_callback(
      ctx,
      [](pa_context* c, void* ud) {
        if (pa_context_get_state(c) == PA_CONTEXT_READY) {
          pa_operation_unref(
              pa_context_get_server_info(c, ServerInfoCb, ud));
        }
      },
      &query);
  if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
    pa_context_unref(ctx);
    pa_mainloop_free(ml);
    return false;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!query.have_spec &&
         std::chrono::steady_clock::now() < deadline) {
    const pa_context_state_t st = pa_context_get_state(ctx);
    if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED) break;
    if (pa_mainloop_iterate(ml, 1, nullptr) < 0) break;
  }
  const bool ok = query.have_spec && query.spec.rate > 0 &&
                  query.spec.channels > 0;
  if (ok) *spec_out = query.spec;
  pa_context_disconnect(ctx);
  pa_context_unref(ctx);
  pa_mainloop_free(ml);
  return ok;
}

pa_simple* OpenAt(uint32_t rate, uint8_t channels, int* error_out) {
  pa_sample_spec ss;
  ss.format = PA_SAMPLE_S16LE;
  ss.rate = rate;
  ss.channels = channels;
  pa_buffer_attr attr;
  std::memset(&attr, 0xff, sizeof(attr));
  const uint32_t bytes_per_sec =
      rate * channels * static_cast<uint32_t>(sizeof(int16_t));
  attr.fragsize = bytes_per_sec / 5;
  int error = 0;
  pa_simple* s =
      pa_simple_new(nullptr, "fluffychat", PA_STREAM_RECORD, nullptr, "voice",
                    &ss, nullptr, &attr, &error);
  *error_out = error;
  return s;
}

pa_simple* OpenCapture(int* rate_out, int* channels_out, int* error_out) {
  // Cinny’s working path is Qt’s default input at the phone’s own PCM
  // format (nearestFormat), not a forced 48 kHz Opus encode.
  pa_sample_spec native{};
  if (QueryDefaultSource(&native)) {
    pa_simple* s =
        OpenAt(native.rate, native.channels, error_out);
    if (s) {
      *rate_out = static_cast<int>(native.rate);
      *channels_out = native.channels;
      return s;
    }
  }
  struct TrySpec {
    uint32_t rate;
    uint8_t channels;
  };
  const TrySpec tries[] = {{44100, 1}, {16000, 1}, {48000, 1}, {48000, 2}};
  for (const auto& spec : tries) {
    pa_simple* s = OpenAt(spec.rate, spec.channels, error_out);
    if (s) {
      *rate_out = static_cast<int>(spec.rate);
      *channels_out = spec.channels;
      return s;
    }
  }
  return nullptr;
}

void PlayBeep();  // defined below

std::atomic<bool> g_playing{false};

bool PlayOpusFile(const std::string& path) {
#ifdef UT_MEDIA_HAVE_OPUSFILE
  int err = 0;
  OggOpusFile* of = op_open_file(path.c_str(), &err);
  if (!of) return false;
  const struct OpusHead* head = op_head(of, -1);
  const int channels = head ? head->channel_count : 1;
  pa_sample_spec ss;
  ss.format = PA_SAMPLE_S16LE;
  ss.rate = 48000;
  ss.channels = static_cast<uint8_t>(std::max(1, channels));
  int error = 0;
  pa_simple* s = pa_simple_new(nullptr, "fluffychat", PA_STREAM_PLAYBACK, nullptr,
                               "voice", &ss, nullptr, nullptr, &error);
  if (!s) {
    op_free(of);
    return false;
  }
  int16_t buf[960 * 8];
  while (g_playing.load()) {
    const int n = op_read(of, buf, 960 * ss.channels, nullptr);
    if (n <= 0) break;
    if (pa_simple_write(s, buf, static_cast<size_t>(n) * ss.channels * sizeof(int16_t),
                        &error) < 0) {
      break;
    }
  }
  if (g_playing.load()) pa_simple_drain(s, &error);
  pa_simple_free(s);
  op_free(of);
  return true;
#else
  (void)path;
  return false;
#endif
}

bool PlayVorbisFile(const std::string& path) {
#ifdef UT_MEDIA_HAVE_VORBIS
  OggVorbis_File vf;
  if (ov_fopen(path.c_str(), &vf) != 0) return false;
  vorbis_info* info = ov_info(&vf, -1);
  if (!info) {
    ov_clear(&vf);
    return false;
  }
  pa_sample_spec ss;
  ss.format = PA_SAMPLE_S16LE;
  ss.rate = static_cast<uint32_t>(info->rate);
  ss.channels = static_cast<uint8_t>(info->channels);
  int error = 0;
  pa_simple* s = pa_simple_new(nullptr, "fluffychat", PA_STREAM_PLAYBACK, nullptr,
                               "voice", &ss, nullptr, nullptr, &error);
  if (!s) {
    ov_clear(&vf);
    return false;
  }
  char pcm[4096];
  int bitstream = 0;
  long n = 0;
  while (g_playing.load() &&
         (n = ov_read(&vf, pcm, sizeof(pcm), 0, 2, 1, &bitstream)) > 0) {
    pa_simple_write(s, pcm, static_cast<size_t>(n), &error);
  }
  if (g_playing.load()) pa_simple_drain(s, &error);
  pa_simple_free(s);
  ov_clear(&vf);
  return true;
#else
  (void)path;
  return false;
#endif
}

void PlayAudioFile(const std::string& path) {
  g_playing = true;
  if (!PlayOpusFile(path)) {
    PlayVorbisFile(path);
  }
  g_playing = false;
}

void StopAudioFile() { g_playing = false; }

void PlayBeep() {
  pa_sample_spec ss;
  ss.format = PA_SAMPLE_S16LE;
  ss.rate = 16000;
  ss.channels = 1;
  int error = 0;
  pa_simple* s = pa_simple_new(nullptr, "fluffychat", PA_STREAM_PLAYBACK, nullptr,
                               "alert", &ss, nullptr, nullptr, &error);
  if (!s) return;
  const int n = 16000 * 12 / 100;
  std::vector<int16_t> buf(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / 16000.0;
    double freq = 1046.5;
    if (i > n / 3 && i <= 2 * n / 3) freq = 1318.5;
    if (i > 2 * n / 3) freq = 1568.0;
    const double env = std::exp(-t * 8.0);
    buf[static_cast<size_t>(i)] =
        static_cast<int16_t>(std::sin(2.0 * 3.141592653589793 * freq * t) *
                             18000.0 * env);
  }
  pa_simple_write(s, buf.data(), buf.size() * sizeof(int16_t), &error);
  pa_simple_drain(s, &error);
  pa_simple_free(s);
}

void PlayXylo() {
#ifdef UT_MEDIA_HAVE_VORBIS
  OggVorbis_File vf;
  if (ov_fopen(kXyloPath, &vf) == 0) {
    vorbis_info* info = ov_info(&vf, -1);
    if (info) {
      pa_sample_spec ss;
      ss.format = PA_SAMPLE_S16LE;
      ss.rate = static_cast<uint32_t>(info->rate);
      ss.channels = static_cast<uint8_t>(info->channels);
      int error = 0;
      pa_simple* s =
          pa_simple_new(nullptr, "fluffychat", PA_STREAM_PLAYBACK, nullptr,
                        "alert", &ss, nullptr, nullptr, &error);
      if (s) {
        char pcm[4096];
        int bitstream = 0;
        long n = 0;
        while ((n = ov_read(&vf, pcm, sizeof(pcm), 0, 2, 1, &bitstream)) > 0) {
          pa_simple_write(s, pcm, static_cast<size_t>(n), &error);
        }
        pa_simple_drain(s, &error);
        pa_simple_free(s);
        ov_clear(&vf);
        return;
      }
    }
    ov_clear(&vf);
  }
#endif
  PlayBeep();
}

void WriteSysfs(const char* path, const char* value) {
  std::ofstream f(path);
  if (f) f << value;
}

void Haptic() {
  WriteSysfs("/sys/class/timed_output/vibrator/enable", "40");
  WriteSysfs("/sys/class/leds/vibrator/duration", "40");
  WriteSysfs("/sys/class/leds/vibrator/activate", "1");

  GError* err = nullptr;
  GDBusConnection* conn =
      g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
  if (!conn) {
    if (err) g_error_free(err);
    return;
  }
  GVariant* reply = g_dbus_connection_call_sync(
      conn, "com.lomiri.hfd", "/com/lomiri/hfd", "com.lomiri.hfd", "vibrate",
      g_variant_new("(u)", 40u), nullptr, G_DBUS_CALL_FLAGS_NONE, 500, nullptr,
      &err);
  if (reply) g_variant_unref(reply);
  if (err) g_error_free(err);
  g_object_unref(conn);
}

}  // namespace

class UtMediaPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar);
  UtMediaPlugin() = default;
  ~UtMediaPlugin() override {
    StopAudioFile();
    Cancel();
  }

  UtMediaPlugin(const UtMediaPlugin&) = delete;
  UtMediaPlugin& operator=(const UtMediaPlugin&) = delete;

 private:
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  bool StartRecording(const std::string& path, std::string* error);
  bool StopRecording(std::string* path_out, std::string* error);
  void Cancel();

  std::mutex mu_;
  std::condition_variable start_cv_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> paused_{false};
  std::atomic<double> amplitude_{-60.0};
  std::vector<int16_t> pcm_;
  std::string path_;
  int capture_rate_ = kEncoderRate;
  int capture_channels_ = 1;
  bool start_done_ = false;
  bool start_ok_ = false;
  std::string start_error_;
};

void UtMediaPlugin::RegisterWithRegistrar(flutter::PluginRegistrar* registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), kChannelName,
          &flutter::StandardMethodCodec::GetInstance());
  auto plugin = std::make_unique<UtMediaPlugin>();
  channel->SetMethodCallHandler(
      [plugin_ptr = plugin.get()](const auto& call, auto result) {
        plugin_ptr->HandleMethodCall(call, std::move(result));
      });
  registrar->AddPlugin(std::move(plugin));
}

bool UtMediaPlugin::StartRecording(const std::string& path, std::string* error) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (running_) {
      *error = "Already recording";
      return false;
    }
    path_ = path;
    pcm_.clear();
    paused_ = false;
    amplitude_ = -60.0;
    capture_rate_ = kEncoderRate;
    capture_channels_ = 1;
    start_done_ = false;
    start_ok_ = false;
    start_error_.clear();
    running_ = true;
  }
  thread_ = std::thread([this]() {
    int rate = 0;
    int channels = 0;
    int error = 0;
    pa_simple* s = OpenCapture(&rate, &channels, &error);
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!s) {
        start_ok_ = false;
        start_error_ = error ? pa_strerror(error) : "No microphone.";
        start_done_ = true;
        running_ = false;
      } else {
        capture_rate_ = rate;
        capture_channels_ = channels;
        start_ok_ = true;
        start_done_ = true;
      }
    }
    start_cv_.notify_one();
    if (!s) return;

    const int frame_samples = std::max(1, rate * kFrameMs / 1000);
    const int read_samples = frame_samples * channels;
    std::vector<int16_t> buf(static_cast<size_t>(read_samples));
    const int max_samples = rate * channels * kMaxSeconds;
    while (running_) {
      int read_err = 0;
      if (pa_simple_read(s, buf.data(), buf.size() * sizeof(int16_t),
                         &read_err) < 0) {
        break;
      }
      double sum = 0;
      const int frames = read_samples / channels;
      for (int i = 0; i < frames; ++i) {
        int acc = 0;
        for (int c = 0; c < channels; ++c) {
          acc += buf[static_cast<size_t>(i * channels + c)];
        }
        const double v = (acc / channels) / 32768.0;
        sum += v * v;
      }
      const double rms = std::sqrt(sum / std::max(1, frames));
      amplitude_ = rms > 1e-9 ? 20.0 * std::log10(rms) : -60.0;
      if (paused_) continue;
      std::lock_guard<std::mutex> lock(mu_);
      pcm_.insert(pcm_.end(), buf.begin(), buf.end());
      if (static_cast<int>(pcm_.size()) > max_samples) {
        running_ = false;
        break;
      }
    }
    pa_simple_free(s);
  });

  std::unique_lock<std::mutex> lock(mu_);
  start_cv_.wait_for(lock, std::chrono::seconds(3), [this] { return start_done_; });
  if (!start_ok_) {
    *error = start_error_.empty() ? "Could not start microphone." : start_error_;
    lock.unlock();
    if (thread_.joinable()) thread_.join();
    running_ = false;
    return false;
  }
  return true;
}

bool UtMediaPlugin::StopRecording(std::string* path_out, std::string* error) {
  std::vector<int16_t> copy;
  std::string path;
  int rate = kEncoderRate;
  int channels = 1;
  {
    std::lock_guard<std::mutex> lock(mu_);
    running_ = false;
    path = path_;
    rate = capture_rate_;
    channels = capture_channels_;
  }
  if (thread_.joinable()) thread_.join();
  {
    std::lock_guard<std::mutex> lock(mu_);
    copy.swap(pcm_);
    path_.clear();
  }
  auto mono = ToMono16(copy, std::max(1, channels));
  mono = ResampleTo16k(mono, rate);
  if (path.empty()) {
    path = WritableDir() + "/voice_message.ogg";
  }
  if (!EncodeOpusFile(mono, path)) {
    const std::string fallback = WritableDir() + "/voice_message.ogg";
    if (fallback != path && EncodeOpusFile(mono, fallback)) {
      *path_out = fallback;
      return true;
    }
    *error = "Could not encode voice message.";
    return false;
  }
  *path_out = path;
  return true;
}

void UtMediaPlugin::Cancel() {
  running_ = false;
  start_cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lock(mu_);
  pcm_.clear();
  path_.clear();
  start_done_ = false;
  start_ok_ = false;
}

void UtMediaPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto& method = call.method_name();
  if (method == "playAlert") {
    std::thread(PlayXylo).detach();
    result->Success(flutter::EncodableValue());
    return;
  }
  if (method == "playFile") {
    const auto* path = std::get_if<std::string>(call.arguments());
    if (!path || path->empty()) {
      result->Error("INVALID_ARGS", "Missing path");
      return;
    }
    StopAudioFile();
    std::thread([p = *path]() { PlayAudioFile(p); }).detach();
    result->Success(flutter::EncodableValue());
    return;
  }
  if (method == "stopPlayback") {
    StopAudioFile();
    result->Success(flutter::EncodableValue());
    return;
  }
  if (method == "haptic") {
    std::thread(Haptic).detach();
    result->Success(flutter::EncodableValue());
    return;
  }
  if (method == "startRecording") {
    const auto* path = std::get_if<std::string>(call.arguments());
    if (!path || path->empty()) {
      result->Error("INVALID_ARGS", "Missing path");
      return;
    }
    std::string error;
    if (!StartRecording(*path, &error)) {
      result->Error("RECORD", error);
      return;
    }
    result->Success(flutter::EncodableValue());
    return;
  }
  if (method == "stopRecording") {
    std::string path;
    std::string error;
    if (!StopRecording(&path, &error)) {
      result->Error("RECORD", error);
      return;
    }
    result->Success(flutter::EncodableValue(path));
    return;
  }
  if (method == "cancelRecording") {
    Cancel();
    result->Success(flutter::EncodableValue());
    return;
  }
  if (method == "pauseRecording") {
    paused_ = true;
    result->Success(flutter::EncodableValue());
    return;
  }
  if (method == "resumeRecording") {
    paused_ = false;
    result->Success(flutter::EncodableValue());
    return;
  }
  if (method == "amplitude") {
    result->Success(flutter::EncodableValue(amplitude_.load()));
    return;
  }
  result->NotImplemented();
}

}  // namespace ut_media

void UtMediaPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  static auto* plugin_registrar = new flutter::PluginRegistrar(registrar);
  ut_media::UtMediaPlugin::RegisterWithRegistrar(plugin_registrar);
}
