// See simple_encoder.c for more details about the encoder.
// The encoder lifetime:
//
//  - vpx_js_encoder_open()
//  - Write frame pixels to /vpx-yuv file.
//  - vpx_js_encoder_run();
//  - vpx_js_encoder_close()
//  - Read IVF packets from /vpx-ivf file.
//
// See simple_decoder.c for more details about the decoder.
// The decoder lifetime:
//
//  - vpx_js_decoder_open()
//  - Make sure the /vpx-ivf file contains IVF packets.
//  - vpx_js_decoder_run();
//  - vpx_js_decoder_close()
//  - Read YUV frames from /vpx-yuv file.
//
// All the files are in-memory memfs emscripten files.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emscripten.h"
#include "../vp8cx.h"
#include "../vp8dx.h"
#include "../vpx_encoder.h"
#include "../vpx_decoder.h"
#include "../../vpx_ports/mem_ops.h"
#include "libyuv/convert.h"
#include "libyuv/convert_from.h"

#define VP8_FOURCC 0x30385056
#define VP9_FOURCC 0x30395056

#define IVF_FRAME_HDR_SZ (4 + 8) /* 4 byte size + 8 byte timestamp */
#define IVF_FILE_HDR_SZ 32

static const char *const kIVFSignature = "DKIF";

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

typedef struct VpxVideoReaderStruct {
  VpxVideoInfo info;
  FILE *file;
  uint8_t *buffer;
  size_t buffer_size;
  size_t frame_size;
} VpxVideoReader;

typedef struct VpxVideoWriterStruct {
  VpxVideoInfo info;
  FILE *file;
  int frame_count;
} VpxVideoWriter;

static const VpxInterface vpx_encoders[] = {
  { "vp8", VP8_FOURCC, &vpx_codec_vp8_cx },
  { "vp9", VP9_FOURCC, &vpx_codec_vp9_cx },
};

static const VpxInterface vpx_decoders[] = {
  { "vp8", VP8_FOURCC, &vpx_codec_vp8_dx },
  { "vp9", VP9_FOURCC, &vpx_codec_vp9_dx },
};

static const char* ivf_file_path = "/vpx-ivf";
static const char* yuv_file_path = "/vpx-yuv";

static FILE *infile = NULL;
static FILE *outfile = NULL;
static vpx_codec_ctx_t codec;
static vpx_codec_enc_cfg_t cfg;
static int frame_count = 0;
static vpx_image_t img;
static vpx_codec_err_t res;
static const VpxVideoInfo *info = NULL;
static VpxVideoWriter *writer = NULL;
static VpxVideoReader *reader = NULL;
static const VpxInterface *encoder = NULL;
static const VpxInterface *decoder = NULL;
static int fps = 30;
static int bitrate = 200;
static int keyframe_interval = 0;
static int max_frames = 0;
static int frames_encoded = 0;

void die(const char* message) {
  printf("die: %s\n", message);
  exit(EXIT_FAILURE);
}

void check_frame_size(int w, int h) {
  if (w <= 0 || h <= 0 || w % 2 || h % 2)
    die("Bad frame size.");
}

int get_vpx_decoder_count(void) {
  return sizeof(vpx_decoders) / sizeof(vpx_decoders[0]);
}

int get_vpx_encoder_count(void) {
  return sizeof(vpx_encoders) / sizeof(vpx_encoders[0]);
}

const VpxInterface* get_vpx_decoder_by_fourcc(uint32_t fourcc) {
  const int n = get_vpx_decoder_count();

  for (int i = 0; i < n; i++) {
    const VpxInterface* e = &vpx_decoders[i];
    if (e->fourcc == fourcc)
      return e;
  }

  return NULL;
}

const VpxInterface* get_vpx_encoder_by_fourcc(uint32_t fourcc) {
  const int n = get_vpx_encoder_count();

  for (int i = 0; i < n; i++) {
    const VpxInterface* e = &vpx_encoders[i];
    if (e->fourcc == fourcc)
      return e;
  }

  return NULL;
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

void vpx_img_write(const vpx_image_t *img, FILE *file) {
  int plane;

  for (plane = 0; plane < 3; ++plane) {
    const unsigned char *buf = img->planes[plane];
    const int stride = img->stride[plane];
    const int w = vpx_img_plane_width(img, plane) *
                  ((img->fmt & VPX_IMG_FMT_HIGHBITDEPTH) ? 2 : 1);
    const int h = vpx_img_plane_height(img, plane);
    int y;

    for (y = 0; y < h; ++y) {
      fwrite(buf, 1, w, file);
      buf += stride;
    }
  }
}

// ivf reader

int ivf_read_frame(FILE *infile, uint8_t **buffer, size_t *bytes_read,
                   size_t *buffer_size) {
  char raw_header[IVF_FRAME_HDR_SZ] = { 0 };
  size_t frame_size = 0;

  // printf("Reading IVF frame from file pos %d\n", (int)ftell(infile));
  if (fread(raw_header, IVF_FRAME_HDR_SZ, 1, infile) != 1) {
    if (!feof(infile))
      printf("Failed to read frame size\n");
    /* else
      printf("Reached end of IVF file.\n"); */
  } else {
    frame_size = mem_get_le32(raw_header);
    // printf("IVF frame size: %d\n", (int)frame_size);

    if (frame_size > 256 * 1024 * 1024) {
      printf("Read invalid frame size (%u)\n", (unsigned int)frame_size);
      frame_size = 0;
    }

    if (frame_size > *buffer_size) {
      uint8_t *new_buffer = realloc(*buffer, 2 * frame_size);

      if (new_buffer) {
        *buffer = new_buffer;
        *buffer_size = 2 * frame_size;
      } else {
        printf("Failed to allocate compressed data buffer\n");
        frame_size = 0;
      }
    }
  }

  if (!feof(infile)) {
    // printf("Reading the frame data.\n");
    if (fread(*buffer, 1, frame_size, infile) != frame_size) {
      printf("Failed to read full frame\n");
      return 1;
    }

    *bytes_read = frame_size;
    return 0;
  }

  // printf("Failed to read the IVF frame.\n");
  return 1;
}

// ivf writer

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

// video reader: ivf -> vp8 -> yuv

VpxVideoReader *vpx_video_reader_open(const char *filename) {
  char header[32];
  VpxVideoReader *reader = NULL;
  FILE *const file = fopen(filename, "rb");
  if (!file) return NULL;  // Can't open file

  if (fread(header, 1, 32, file) != 32) return NULL;  // Can't read file header

  if (memcmp(kIVFSignature, header, 4) != 0)
    return NULL;  // Wrong IVF signature

  if (mem_get_le16(header + 4) != 0) return NULL;  // Wrong IVF version

  reader = calloc(1, sizeof(*reader));
  if (!reader) return NULL;  // Can't allocate VpxVideoReader

  reader->file = file;
  reader->info.codec_fourcc = mem_get_le32(header + 8);
  reader->info.frame_width = mem_get_le16(header + 12);
  reader->info.frame_height = mem_get_le16(header + 14);
  reader->info.time_base.numerator = mem_get_le32(header + 16);
  reader->info.time_base.denominator = mem_get_le32(header + 20);

  return reader;
}

void vpx_video_reader_close(VpxVideoReader *reader) {
  fclose(reader->file);
  free(reader->buffer);
  free(reader);
}

int vpx_video_reader_read_frame(VpxVideoReader *reader) {
  return !ivf_read_frame(reader->file, &reader->buffer, &reader->frame_size,
                         &reader->buffer_size);
}

const uint8_t *vpx_video_reader_get_frame(VpxVideoReader *reader,
                                          size_t *size) {
  if (size) *size = reader->frame_size;

  return reader->buffer;
}

const VpxVideoInfo *vpx_video_reader_get_info(VpxVideoReader *reader) {
  return &reader->info;
}

// video writer: yuv -> vp8 -> ivf

void write_header(FILE *file, const VpxVideoInfo *info,
                         int frame_count) {
  struct vpx_codec_enc_cfg cfg;
  cfg.g_w = info->frame_width;
  cfg.g_h = info->frame_height;
  cfg.g_timebase.num = info->time_base.numerator;
  cfg.g_timebase.den = info->time_base.denominator;

  ivf_write_file_header(file, &cfg, info->codec_fourcc, frame_count);
}

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

void vpx_video_writer_update_header(VpxVideoWriter *writer) {
  // Rewriting frame header with real frame count
  int pos = ftell(writer->file);
  rewind(writer->file);
  write_header(writer->file, &writer->info, writer->frame_count);
  fseek(writer->file, pos, SEEK_SET); // restore the original position
}

void vpx_video_writer_close(VpxVideoWriter *writer) {
  vpx_video_writer_update_header(writer);
  fclose(writer->file);
  free(writer);
}

int encode_frame(vpx_codec_ctx_t *codec, vpx_image_t *img,
                 int frame_index, int flags , VpxVideoWriter *writer) {
  int got_pkts = 0;
  vpx_codec_iter_t iter = NULL;
  const vpx_codec_cx_pkt_t *pkt = NULL;

  if (vpx_codec_encode(codec, img, frame_index, 1, flags, VPX_DL_REALTIME))
    die("Failed to encode frame");

  while ((pkt = vpx_codec_get_cx_data(codec, &iter)) != NULL) {
    got_pkts = 1;

    if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
      const int keyframe = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
      if (!vpx_video_writer_write_frame(writer, pkt->data.frame.buf,
                                        pkt->data.frame.sz,
                                        pkt->data.frame.pts)) {
        die("Failed to write compressed frame");
      }
      printf("Encoded a %s\n", keyframe ? "key-frame" : "delta-frame");
      fflush(stdout);
    }
  }

  return got_pkts;
}

// JS API

EMSCRIPTEN_KEEPALIVE
void vpx_js_decoder_open() {
  reader = vpx_video_reader_open(ivf_file_path);
  if (!reader) die("Failed to open IVF file for reading.");

  info = vpx_video_reader_get_info(reader);
  decoder = get_vpx_decoder_by_fourcc(info->codec_fourcc);
  if (!decoder) die("Unknown input codec.");

  printf("Using %s\n", vpx_codec_iface_name(decoder->codec_interface()));

  if (vpx_codec_dec_init(&codec, decoder->codec_interface(), NULL, 0))
    die("Failed to initialize decoder.");

  printf("Reading IVF from %s, writing YUV to %s\n",
    ivf_file_path, yuv_file_path);
}

EMSCRIPTEN_KEEPALIVE
void vpx_js_encoder_open(uint32_t fourcc, int frame_width, int frame_height) {
  if (keyframe_interval < 0) die("Invalid keyframe interval value.");
  check_frame_size(frame_width, frame_height);

  encoder = get_vpx_encoder_by_fourcc(fourcc);
  if (!encoder) die("Invalid codec fourcc");
  printf("Using %s\n", vpx_codec_iface_name(encoder->codec_interface()));

  // init ivf writer

  VpxVideoInfo info = { 0, 0, 0, { 0, 0 } };

  info.codec_fourcc = encoder->fourcc;
  info.frame_width = frame_width;
  info.frame_height = frame_height;
  info.time_base.numerator = 1;
  info.time_base.denominator = fps;

  writer = vpx_video_writer_open(ivf_file_path, kContainerIVF, &info);
  if (!writer) die("Failed to open %s for writing.");

  // init img buffer

  if (!vpx_img_alloc(&img, VPX_IMG_FMT_I420, frame_width, frame_height, 1)) {
    die("Failed to allocate image.");
  }

  // init vpx encoder

  res = vpx_codec_enc_config_default(encoder->codec_interface(), &cfg, 0);
  if (res) die("Failed to get default codec config.");

  cfg.g_w = frame_width;
  cfg.g_h = frame_height;
  cfg.g_timebase.num = 1;
  cfg.g_timebase.den = fps;
  cfg.rc_target_bitrate = bitrate;
  cfg.g_error_resilient = (vpx_codec_er_flags_t)0;

  if (vpx_codec_enc_init(&codec, encoder->codec_interface(), &cfg, 0))
    die("Failed to initialize encoder");

  printf("Reading YUV from %s, writing IVF to %s\n",
    yuv_file_path, ivf_file_path);
}

EMSCRIPTEN_KEEPALIVE
void vpx_js_decoder_close() {
  if (vpx_codec_destroy(&codec))
    die("Failed to destroy codec.");

  printf("Play: ffplay -f rawvideo -pix_fmt yuv420p -s %dx%d %s\n",
    info->frame_width, info->frame_height, yuv_file_path);

  vpx_video_reader_close(reader);
  fclose(outfile);
}

EMSCRIPTEN_KEEPALIVE
void vpx_js_encoder_close() {
  vpx_img_free(&img);
  vpx_video_writer_close(writer);

  if (vpx_codec_destroy(&codec))
    die("Failed to destroy codec.");
}

EMSCRIPTEN_KEEPALIVE
void vpx_js_decoder_run() {
  FILE* outfile = fopen(yuv_file_path, "wb");
  if (!outfile) die("Failed to open YUV file for writing.");

  int pos = ftell(reader->file);
  printf("Reopening the IVF file at pos %d\n", pos);
  fclose(reader->file);
  reader->file = fopen(ivf_file_path, "rb");
  fseek(reader->file, pos, SEEK_SET);

  while (vpx_video_reader_read_frame(reader)) {
    vpx_codec_iter_t iter = NULL;
    vpx_image_t *img = NULL;
    size_t frame_size = 0;
    const uint8_t *frame = NULL;

    frame = vpx_video_reader_get_frame(reader, &frame_size);
    printf("Got an IVF frame: %d bytes\n", (int)frame_size);

    if (vpx_codec_decode(&codec, frame, (unsigned int)frame_size, NULL, 0))
      die("Failed to decode frame.");

    while ((img = vpx_codec_get_frame(&codec, &iter)) != NULL) {
      printf("Decoded a YUV frame: %dx%d.\n", img->w, img->h);
      vpx_img_write(img, outfile);
      ++frame_count;
    }
  }

  fclose(outfile);
  fflush(stdout);
}

EMSCRIPTEN_KEEPALIVE
void vpx_js_encoder_run() {
  if (!(infile = fopen(yuv_file_path, "rb")))
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

  vpx_video_writer_update_header(writer);
  fflush(writer->file); // make IVF packets readable from /vpx-ivf
  fclose(infile);
}

// yuv = malloc(width * height * 3/2);
// rgba = malloc(width * height * 4);
EMSCRIPTEN_KEEPALIVE
int vpx_js_rgba_to_yuv420(
  uint8_t* yuv, uint8_t* rgba, int width, int height) {
  // Taken from WebRTC's ConvertRGB24ToI420:
  uint8_t* yplane = yuv;
  uint8_t* uplane = yplane + width * height;
  uint8_t* vplane = uplane + width * height / 4;

  return ABGRToI420(
    rgba, width * 4,
    yplane, width,
    uplane, width / 2,
    vplane, width / 2,
    width, height);
}

// yuv = malloc(width * height * 3/2);
// rgba = malloc(width * height * 4);
EMSCRIPTEN_KEEPALIVE
int vpx_js_yuv420_to_rgba(
  uint8_t* rgba, uint8_t* yuv, int width, int height) {
  // Taken from WebRTC's ConvertRGB24ToI420:
  uint8_t* yplane = yuv;
  uint8_t* uplane = yplane + width * height;
  uint8_t* vplane = uplane + width * height / 4;

  return I420ToABGR(
    yplane, width,
    uplane, width / 2,
    vplane, width / 2,
    rgba, width * 4,
    width, height);
}
