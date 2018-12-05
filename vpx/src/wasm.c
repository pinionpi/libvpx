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

const char* ENC_IVF_FILE = "/vpx-enc-ivf"; // vpx encoder writes here
const char* ENC_YUV_FILE = "/vpx-enc-yuv"; // vpx encoder read here
const char* DEC_IVF_FILE = "/vpx-dec-ivf"; // vpx decoder reads here

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

typedef struct {
  VpxVideoInfo info;
  FILE *file;
  uint8_t *buffer;
  size_t buffer_size;
  size_t frame_size;
} VpxVideoReader;

typedef struct {
  VpxVideoInfo info;
  FILE *file;
  vpx_image_t img;
  int frame_count;
  int keyframe_interval;
} VpxVideoWriter;

const VpxInterface vpx_encoders[] = {
  { "vp8", VP8_FOURCC, &vpx_codec_vp8_cx },
  { "vp9", VP9_FOURCC, &vpx_codec_vp9_cx },
};

const VpxInterface vpx_decoders[] = {
  { "vp8", VP8_FOURCC, &vpx_codec_vp8_dx },
  { "vp9", VP9_FOURCC, &vpx_codec_vp9_dx },
};

vpx_codec_ctx_t ctx_enc;
vpx_codec_ctx_t ctx_dec;
VpxVideoWriter *writer = NULL;
VpxVideoReader *reader = NULL;
const VpxInterface *encoder = NULL;
const VpxInterface *decoder = NULL;

#define die(args) { printf args; exit(EXIT_FAILURE); }

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

// ivf reader + writer

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

VpxVideoReader *vpx_video_reader_open(const VpxVideoInfo *info) {
  VpxVideoReader *reader = malloc(sizeof(*reader));
  reader->file = NULL;
  reader->info = *info;
  return reader;
}

void vpx_video_reader_close(VpxVideoReader *reader) {
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

// video writer: yuv -> vp8 -> ivf

VpxVideoWriter *vpx_video_writer_open(const VpxVideoInfo *info) {
  VpxVideoWriter* writer = malloc(sizeof(*writer));

  if (!vpx_img_alloc(&writer->img, VPX_IMG_FMT_I420,
    info->frame_width, info->frame_height, 1))
    die(("Failed to allocate image."));

  writer->keyframe_interval = 0;
  writer->frame_count = 0;
  writer->info = *info;
  writer->file = NULL;
  return writer;
}

int vpx_video_writer_write_frame(VpxVideoWriter *writer, const uint8_t *buffer,
                                 size_t size, int64_t pts) {
  ivf_write_frame_header(writer->file, pts, size);
  if (fwrite(buffer, 1, size, writer->file) != size) return 0;
  ++writer->frame_count;
  return 1;
}

void vpx_video_writer_close(VpxVideoWriter *writer) {
  vpx_img_free(&writer->img);
  free(writer);
}

int encode_frame(vpx_image_t *img, int frame_index, int flags,
  VpxVideoWriter *writer) {

  int got_pkts = 0;
  vpx_codec_iter_t iter = NULL;
  const vpx_codec_cx_pkt_t *pkt = NULL;

  int res = vpx_codec_encode(&ctx_enc, img, frame_index, 1, flags, VPX_DL_REALTIME);
  if (res) die(("vpx_codec_encode failed: %d\n", (int)res));

  while ((pkt = vpx_codec_get_cx_data(&ctx_enc, &iter)) != NULL) {
    got_pkts = 1;

    if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
      if (!vpx_video_writer_write_frame(writer, pkt->data.frame.buf,
                                        pkt->data.frame.sz,
                                        pkt->data.frame.pts)) {
        die(("Failed to write compressed frame"));
      }

      if (pkt->data.frame.flags & VPX_FRAME_IS_KEY)
        printf("Created a keyframe\n");
    }
  }

  return got_pkts;
}

// JS API

EMSCRIPTEN_KEEPALIVE
void vpx_js_decoder_open(uint32_t fourcc, int width, int height, int fps) {
  VpxVideoInfo info = { 0 };

  info.codec_fourcc = fourcc;
  info.frame_width = width;
  info.frame_height = height;
  info.time_base.numerator = 1;
  info.time_base.denominator = fps;

  reader = vpx_video_reader_open(&info);
  if (!reader) die(("Failed to open IVF file for reading."));

  decoder = get_vpx_decoder_by_fourcc(reader->info.codec_fourcc);
  if (!decoder) die(("Unknown input codec."));

  printf("Using %s\n", vpx_codec_iface_name(decoder->codec_interface()));

  vpx_codec_dec_cfg_t cfg = { 0 };

  cfg.w = width;
  cfg.h = height;

  if (vpx_codec_dec_init(&ctx_dec, decoder->codec_interface(), &cfg, 0))
    die(("Failed to initialize decoder."));
}

EMSCRIPTEN_KEEPALIVE
void vpx_js_encoder_open(uint32_t fourcc, int width, int height, int fps, int bitrate) {
  encoder = get_vpx_encoder_by_fourcc(fourcc);
  if (!encoder) die(("Invalid codec fourcc: 0x%x\n", fourcc));
  printf("Using %s\n", vpx_codec_iface_name(encoder->codec_interface()));

  // init ivf writer

  VpxVideoInfo info = { 0 };

  info.codec_fourcc = encoder->fourcc;
  info.frame_width = width;
  info.frame_height = height;
  info.time_base.numerator = 1;
  info.time_base.denominator = fps;

  writer = vpx_video_writer_open(&info);
  if (!writer) die(("Failed to create the video writer."));

  // init vpx encoder

  vpx_codec_enc_cfg_t cfg;

  if (vpx_codec_enc_config_default(encoder->codec_interface(), &cfg, 0))
    die(("Failed to get default codec config."));

  cfg.g_w = width;
  cfg.g_h = height;
  cfg.g_timebase.num = 1;
  cfg.g_timebase.den = fps;
  cfg.rc_target_bitrate = bitrate; // kbit/s
  cfg.g_error_resilient = (vpx_codec_er_flags_t)0;

  int res = vpx_codec_enc_init(&ctx_enc, encoder->codec_interface(), &cfg, 0);
  if (res) die(("vpx_codec_enc_init failed: %d\n", (int)res))

  printf("Encoding %dx%d from %s to %s\n",
    width, height, ENC_YUV_FILE, ENC_IVF_FILE);
}

EMSCRIPTEN_KEEPALIVE
void vpx_js_decoder_close() {
  vpx_video_reader_close(reader);
  if (vpx_codec_destroy(&ctx_dec))
    printf("vpx_codec_destroy failed");
}

EMSCRIPTEN_KEEPALIVE
void vpx_js_encoder_close() {
  vpx_video_writer_close(writer);
  if (vpx_codec_destroy(&ctx_enc))
    printf("vpx_codec_destroy failed");
}

// rgba = malloc(width*height*4);
EMSCRIPTEN_KEEPALIVE
void vpx_js_decoder_run(uint8_t *rgba) {
  reader->file = fopen(DEC_IVF_FILE, "rb");
  if (!reader->file) die(("Failed to open file: %s\n", DEC_IVF_FILE));

  int width = reader->info.frame_width;
  int height = reader->info.frame_height;

  while (vpx_video_reader_read_frame(reader)) {
    vpx_codec_iter_t iter = NULL;
    vpx_image_t *img = NULL;
    size_t frame_size = 0;
    const uint8_t *frame = NULL;

    frame = vpx_video_reader_get_frame(reader, &frame_size);
    // printf("Got an IVF frame: %d bytes\n", (int)frame_size);

    int res = vpx_codec_decode(&ctx_dec, frame, frame_size, NULL, 0);
    if (res) die(("vpx_codec_decode failed: %d\n", (int)res));

    while ((img = vpx_codec_get_frame(&ctx_dec, &iter)) != NULL) {
      // YUV frame can be 704x544 even if 640x480 is expected.
      // TODO: Scale the YUV image to the frame size first.
      // printf("Decoded a YUV frame: %dx%d.\n", img->w, img->h);
      I420ToABGR(
        img->planes[0], img->stride[0],
        img->planes[1], img->stride[1],
        img->planes[2], img->stride[2],
        rgba, width * 4,
        width, height);
    }
  }

  fclose(reader->file);
}

// The output delta frame (or key frame) size = bitrate / fps.
EMSCRIPTEN_KEEPALIVE
void vpx_js_encoder_run(int force_keyframe) {
  FILE *infile = fopen(ENC_YUV_FILE, "rb");
  if (!infile) die(("Failed to open file: %s\n", ENC_YUV_FILE));

  writer->file = fopen(ENC_IVF_FILE, "wb");
  if (!writer->file) die(("Failed to open file: %s\n", ENC_IVF_FILE));

  int flags = force_keyframe ? VPX_EFLAG_FORCE_KF : 0;
  while (vpx_img_read(&writer->img, infile)) {
    encode_frame(&writer->img, writer->frame_count++, flags , writer);
    // printf("Encoded a YUV frame: %dx%d.\n", writer->img.w, writer->img.h);
  }

  // Flush encoder.
  // while (encode_frame(NULL, -1, 0, writer)) {}

  fclose(writer->file);
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
