#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emscripten.h"
#include "../vp8cx.h"
#include "../vpx_encoder.h"
#include "../../vpx_ports/mem_ops.h"

#define VP8_FOURCC 0x30385056
#define VP9_FOURCC 0x30395056

typedef enum { kContainerIVF } VpxContainer;

typedef struct VpxInterface {
  const char *const name;
  const uint32_t fourcc;
  vpx_codec_iface_t *(*const codec_interface)();
} VpxInterface;

typedef struct {
  int numerator;
  int denominator;
} VpxRational;

typedef struct {
  uint32_t codec_fourcc;
  int frame_width;
  int frame_height;
  VpxRational time_base;
} VpxVideoInfo;

typedef struct VpxVideoWriterStruct {
  VpxVideoInfo info;
  FILE *file;
  int frame_count;
} VpxVideoWriter;

static const VpxInterface vpx_encoders[] = {
  { "vp8", VP8_FOURCC, &vpx_codec_vp8_cx },
  { "vp9", VP9_FOURCC, &vpx_codec_vp9_cx },
};

static const char* outfile_arg = "vpx-output";
static const char* infile_arg = "vpx-input";

static FILE *infile = NULL;
static vpx_codec_ctx_t codec;
static vpx_codec_enc_cfg_t cfg;
static int frame_count = 0;
static vpx_image_t img;
static vpx_codec_err_t res;
static VpxVideoInfo info = { 0, 0, 0, { 0, 0 } };
static VpxVideoWriter *writer = NULL;
static const VpxInterface *encoder = NULL;
static int fps = 30;
static int bitrate = 200;
static int keyframe_interval = 0;
static int max_frames = 0;
static int frames_encoded = 0;

void die(const char* message) {
  printf("die: %s\n", message);
  exit(EXIT_FAILURE);
}

// img encoder

int vpx_img_plane_width(const vpx_image_t *img, int plane) {
  if (plane > 0 && img->x_chroma_shift > 0)
    return (img->d_w + 1) >> img->x_chroma_shift;
  else
    return img->d_w;
}

int vpx_img_plane_height(const vpx_image_t *img, int plane) {
  if (plane > 0 && img->y_chroma_shift > 0)
    return (img->d_h + 1) >> img->y_chroma_shift;
  else
    return img->d_h;
}

int vpx_img_read(vpx_image_t *img, FILE *file) {
  int plane;

  for (plane = 0; plane < 3; ++plane) {
    unsigned char *buf = img->planes[plane];
    const int stride = img->stride[plane];
    const int w = vpx_img_plane_width(img, plane) *
                  ((img->fmt & VPX_IMG_FMT_HIGHBITDEPTH) ? 2 : 1);
    const int h = vpx_img_plane_height(img, plane);
    int y;

    for (y = 0; y < h; ++y) {
      if (fread(buf, 1, w, file) != (size_t)w) return 0;
      buf += stride;
    }
  }

  return 1;
}

// ivf encoder

void ivf_write_file_header(FILE *outfile, const struct vpx_codec_enc_cfg *cfg,
                           unsigned int fourcc, int frame_cnt) {
  char header[32];

  header[0] = 'D';
  header[1] = 'K';
  header[2] = 'I';
  header[3] = 'F';
  mem_put_le16(header + 4, 0);                     // version
  mem_put_le16(header + 6, 32);                    // header size
  mem_put_le32(header + 8, fourcc);                // fourcc
  mem_put_le16(header + 12, cfg->g_w);             // width
  mem_put_le16(header + 14, cfg->g_h);             // height
  mem_put_le32(header + 16, cfg->g_timebase.den);  // rate
  mem_put_le32(header + 20, cfg->g_timebase.num);  // scale
  mem_put_le32(header + 24, frame_cnt);            // length
  mem_put_le32(header + 28, 0);                    // unused

  fwrite(header, 1, 32, outfile);
}

void ivf_write_frame_header(FILE *outfile, int64_t pts, size_t frame_size) {
  char header[12];

  mem_put_le32(header, (int)frame_size);
  mem_put_le32(header + 4, (int)(pts & 0xFFFFFFFF));
  mem_put_le32(header + 8, (int)(pts >> 32));
  fwrite(header, 1, 12, outfile);
}

void ivf_write_frame_size(FILE *outfile, size_t frame_size) {
  char header[4];

  mem_put_le32(header, (int)frame_size);
  fwrite(header, 1, 4, outfile);
}

void write_header(FILE *file, const VpxVideoInfo *info,
                         int frame_count) {
  struct vpx_codec_enc_cfg cfg;
  cfg.g_w = info->frame_width;
  cfg.g_h = info->frame_height;
  cfg.g_timebase.num = info->time_base.numerator;
  cfg.g_timebase.den = info->time_base.denominator;

  ivf_write_file_header(file, &cfg, info->codec_fourcc, frame_count);
}

// video writer: img -> vp8 -> ivf

VpxVideoWriter *vpx_video_writer_open(const char *filename,
                                      VpxContainer container,
                                      const VpxVideoInfo *info) {
  if (container == kContainerIVF) {
    VpxVideoWriter *writer = NULL;
    FILE *const file = fopen(filename, "wb");
    if (!file) return NULL;

    writer = malloc(sizeof(*writer));
    if (!writer) return NULL;

    writer->frame_count = 0;
    writer->info = *info;
    writer->file = file;

    write_header(writer->file, info, 0);

    return writer;
  }

  return NULL;
}

int vpx_video_writer_write_frame(VpxVideoWriter *writer, const uint8_t *buffer,
                                 size_t size, int64_t pts) {
  ivf_write_frame_header(writer->file, pts, size);
  if (fwrite(buffer, 1, size, writer->file) != size) return 0;

  ++writer->frame_count;

  return 1;
}

int encode_frame(vpx_codec_ctx_t *codec, vpx_image_t *img,
                 int frame_index, int flags , VpxVideoWriter *writer) {
  int got_pkts = 0;
  vpx_codec_iter_t iter = NULL;
  const vpx_codec_cx_pkt_t *pkt = NULL;
  const vpx_codec_err_t res =
      vpx_codec_encode(codec, img, frame_index, 1, flags, VPX_DL_GOOD_QUALITY);
  if (res != VPX_CODEC_OK) die("Failed to encode frame");

  while ((pkt = vpx_codec_get_cx_data(codec, &iter)) != NULL) {
    got_pkts = 1;

    if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
      const int keyframe = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
      if (!vpx_video_writer_write_frame(writer, pkt->data.frame.buf,
                                        pkt->data.frame.sz,
                                        pkt->data.frame.pts)) {
        die("Failed to write compressed frame");
      }
      printf(keyframe ? "K" : ".");
      fflush(stdout);
    }
  }

  return got_pkts;
}

// JS API

EMSCRIPTEN_KEEPALIVE
void vpx_js_encoder_init(int frame_width, int frame_height) {
  encoder = &vpx_encoders[0]; // VP8
  printf("Using %s\n", vpx_codec_iface_name(encoder->codec_interface()));

  info.codec_fourcc = encoder->fourcc;
  info.frame_width = frame_width;
  info.frame_height = frame_height;
  info.time_base.numerator = 1;
  info.time_base.denominator = fps;

  if (info.frame_width <= 0 || info.frame_height <= 0 ||
      (info.frame_width % 2) != 0 || (info.frame_height % 2) != 0) {
    die("Invalid frame size");
  }

  if (!vpx_img_alloc(&img, VPX_IMG_FMT_I420, frame_width, frame_height, 1)) {
    die("Failed to allocate image.");
  }

  if (keyframe_interval < 0)
    die("Invalid keyframe interval value.");

  res = vpx_codec_enc_config_default(encoder->codec_interface(), &cfg, 0);
  if (res) die("Failed to get default codec config.");

  cfg.g_w = frame_width;
  cfg.g_h = frame_height;
  cfg.g_timebase.num = 1;
  cfg.g_timebase.den = fps;
  cfg.rc_target_bitrate = bitrate;
  cfg.g_error_resilient = (vpx_codec_er_flags_t)0;

  writer = vpx_video_writer_open(outfile_arg, kContainerIVF, &info);
  if (!writer) die("Failed to open %s for writing.");

  if (vpx_codec_enc_init(&codec, encoder->codec_interface(), &cfg, 0))
    die("Failed to initialize encoder");
}

EMSCRIPTEN_KEEPALIVE
void vpx_js_encoder_send_frame() {
  if (!(infile = fopen(infile_arg, "rb")))
    die("Failed to open file for reading.");

  while (vpx_img_read(&img, infile)) {
    int flags = 0;
    if (keyframe_interval > 0 && frame_count % keyframe_interval == 0)
      flags |= VPX_EFLAG_FORCE_KF;
    encode_frame(&codec, &img, frame_count++, flags , writer);
    frames_encoded++;
    if (max_frames > 0 && frames_encoded >= max_frames) break;
  }

  // Flush encoder.
  while (encode_frame(&codec, NULL, -1, 0, writer)) {}

  fclose(infile);
  printf("Processed %d frames.\n", frame_count);
}
