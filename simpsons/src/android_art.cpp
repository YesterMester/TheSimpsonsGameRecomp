// Pictures for the Android launcher (LauncherArt.java): frames from the game's
// own cutscenes, like the desktop launcher takes with ffmpeg. The cutscenes are
// EA multimedia files: chunks of a four-letter tag and a size, with the VP6
// video header in MVhd and its frames in MV0K (key) and MV0F chunks. FFmpeg's
// VP6 decoder decodes them; its frames are read from the start, since VP6
// can't start in the middle of a stream.

#include <jni.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
extern AVCodec ff_vp6_decoder;
}

namespace {

uint32_t Tag(const char (&name)[5]) {
  return uint32_t(uint8_t(name[0])) | uint32_t(uint8_t(name[1])) << 8 |
         uint32_t(uint8_t(name[2])) << 16 | uint32_t(uint8_t(name[3])) << 24;
}

uint32_t Read32(const uint8_t* p, bool big_endian) {
  return big_endian ? uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]
                    : uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}

struct File {
  FILE* file = nullptr;
  ~File() {
    if (file)
      std::fclose(file);
  }
};

// The frame as ARGB, after its width and height.
std::vector<int32_t> ToArgb(const AVFrame* frame) {
  const int width = frame->width, height = frame->height;
  std::vector<int32_t> out(2 + size_t(width) * height);
  out[0] = width;
  out[1] = height;
  for (int y = 0; y < height; ++y) {
    const uint8_t* luma = frame->data[0] + ptrdiff_t(y) * frame->linesize[0];
    const uint8_t* u = frame->data[1] + ptrdiff_t(y / 2) * frame->linesize[1];
    const uint8_t* v = frame->data[2] + ptrdiff_t(y / 2) * frame->linesize[2];
    int32_t* row = out.data() + 2 + size_t(y) * width;
    for (int x = 0; x < width; ++x) {
      // BT.601, video range.
      const int c = (int(luma[x]) - 16) * 298, d = int(u[x / 2]) - 128, e = int(v[x / 2]) - 128;
      const int r = std::clamp((c + 409 * e + 128) >> 8, 0, 255);
      const int g = std::clamp((c - 100 * d - 208 * e + 128) >> 8, 0, 255);
      const int b = std::clamp((c + 516 * d + 128) >> 8, 0, 255);
      row[x] = int32_t(0xFF000000u | uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b));
    }
  }
  return out;
}

// Frames at the given times (seconds), or empty where the movie ends first.
std::vector<std::vector<int32_t>> Frames(const char* path, const std::vector<double>& seconds) {
  std::vector<std::vector<int32_t>> result(seconds.size());
  File in{std::fopen(path, "rb")};
  if (!in.file || seconds.empty())
    return result;
  const double last = *std::max_element(seconds.begin(), seconds.end());
  std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> codec(
      avcodec_alloc_context3(&ff_vp6_decoder), [](AVCodecContext* c) { avcodec_free_context(&c); });
  std::unique_ptr<AVFrame, void (*)(AVFrame*)> frame(av_frame_alloc(),
                                                     [](AVFrame* f) { av_frame_free(&f); });
  std::unique_ptr<AVPacket, void (*)(AVPacket*)> packet(av_packet_alloc(),
                                                        [](AVPacket* p) { av_packet_free(&p); });
  if (!codec || !frame || !packet)
    return result;
  codec->thread_count = 1;
  if (avcodec_open2(codec.get(), &ff_vp6_decoder, nullptr) < 0)
    return result;
  double frame_rate = 0;
  bool big_endian = false, first = true;
  int64_t decoded = 0;
  std::vector<uint8_t> data;
  uint8_t header[8];
  while (std::fread(header, 1, 8, in.file) == 8) {
    const uint32_t tag = Read32(header, false);
    uint32_t size = Read32(header + 4, false);
    if (first) {
      // The first chunk's size tells the file's byte order.
      big_endian = size > __builtin_bswap32(size);
      first = false;
    }
    if (big_endian)
      size = __builtin_bswap32(size);
    if (size < 8 || size > (64u << 20))
      break;
    size -= 8;
    const bool video = tag == Tag("MV0K") || tag == Tag("MV0F");
    if (tag != Tag("MVhd") && !video) {
      if (std::fseek(in.file, long(size), SEEK_CUR) != 0)
        break;
      continue;
    }
    data.resize(size + AV_INPUT_BUFFER_PADDING_SIZE);
    std::fill(data.begin() + size, data.end(), 0);
    if (std::fread(data.data(), 1, size, in.file) != size)
      break;
    if (tag == Tag("MVhd")) {
      // "vp60", width, height, the frame count, then the rate as den/num.
      if (size >= 24) {
        const uint32_t den = Read32(data.data() + 16, false), num = Read32(data.data() + 20, false);
        if (den && num)
          frame_rate = double(den) / num;
      }
      continue;
    }
    if (frame_rate <= 0)
      frame_rate = 15;
    packet->data = data.data();
    packet->size = int(size);
    if (avcodec_send_packet(codec.get(), packet.get()) < 0)
      continue;
    while (avcodec_receive_frame(codec.get(), frame.get()) == 0) {
      const double time = double(decoded++) / frame_rate;
      if (frame->format == AV_PIX_FMT_YUV420P) {
        for (size_t i = 0; i < seconds.size(); ++i)
          if (result[i].empty() && time >= seconds[i])
            result[i] = ToArgb(frame.get());
      }
      av_frame_unref(frame.get());
    }
    if (std::none_of(result.begin(), result.end(), [](const auto& f) { return f.empty(); }) ||
        double(decoded) / frame_rate > last + 1)
      break;
  }
  return result;
}

}  // namespace

extern "C" JNIEXPORT jobjectArray JNICALL
Java_io_github_yestermester_simpsons_LauncherArt_nativeFrames(JNIEnv* env, jclass, jstring path,
                                                             jdoubleArray times) {
  const char* file = env->GetStringUTFChars(path, nullptr);
  std::vector<double> seconds(size_t(env->GetArrayLength(times)));
  env->GetDoubleArrayRegion(times, 0, jsize(seconds.size()), seconds.data());
  const auto frames = Frames(file, seconds);
  env->ReleaseStringUTFChars(path, file);
  jobjectArray result = env->NewObjectArray(jsize(frames.size()), env->FindClass("[I"), nullptr);
  for (size_t i = 0; result && i < frames.size(); ++i) {
    if (frames[i].empty())
      continue;
    jintArray pixels = env->NewIntArray(jsize(frames[i].size()));
    if (!pixels)
      break;
    env->SetIntArrayRegion(pixels, 0, jsize(frames[i].size()), frames[i].data());
    env->SetObjectArrayElement(result, jsize(i), pixels);
    env->DeleteLocalRef(pixels);
  }
  return result;
}
