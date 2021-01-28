// See simple_encoder.c for more details about the encoder.
// The encoder lifetime:
//
//  - vpx_js_encoder_open()
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
  uint8_t *buffer;
  size_t buffer_size;
  size_t frame_size;
} VpxVideoReader;

typedef struct {
  VpxVideoInfo info;
  vpx_image_t img;
  int frame_count;
  int keyframe_interval;
} VpxVideoWriter;

typedef struct {
  uint8_t *data;
  size_t pos;
  size_t size;
} MFILE;

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

MFILE mfopen(uint8_t *data, int size) {
  MFILE mfile = { 0 };
  mfile.data = data;
  mfile.size = size;
  return mfile;
}

void mfwrite(const void *data, size_t size, MFILE *mfile) {
  if (mfile->pos + size >= mfile->size)
    die(("The output buffer is too small: %zu\n", mfile->size));
  memcpy(&mfile->data[mfile->pos], data, size);
  mfile->pos += size;
}

void mfread(void *data, size_t size, MFILE *mfile) {
  if (mfile->pos + size >= mfile->size)
    die(("The input buffer is too small: %zu\n", mfile->size));
  memcpy(data, &mfile->data[mfile->pos], size);
  mfile->pos += size;
}

// ivf reader + writer

void ivf_read_frame(MFILE *infile, uint8_t **buffer, size_t *bytes_read,
                   size_t *buffer_size) {
  char raw_header[IVF_FRAME_HDR_SZ] = { 0 };
  // printf("Reading IVF frame from file pos %d\n", (int)ftell(infile));
  mfread(raw_header, IVF_FRAME_HDR_SZ, infile);
  size_t frame_size = mem_get_le32(raw_header);
  // printf("IVF frame size: %d\n", (int)frame_size);

  if (frame_size > 256 * 1024 * 1024)
    die(("Read invalid frame size: %zu\n", frame_size));

  if (frame_size > *buffer_size) {
    uint8_t *new_buffer = realloc(*buffer, 2 * frame_size);
    if (!new_buffer) die(("Failed to allocate compressed data buffer\n"));
    *buffer = new_buffer;
    *buffer_size = 2 * frame_size;
  }

  // printf("Reading the frame data.\n");
  mfread(*buffer, frame_size, infile);
  *bytes_read = frame_size;
}

void ivf_write_frame_header(MFILE *output, int64_t pts, size_t frame_size) {
  char header[12];
  mem_put_le32(header, (int)frame_size);
  mem_put_le32(header + 4, (int)(pts & 0xFFFFFFFF));
  mem_put_le32(header + 8, (int)(pts >> 32));
  mfwrite(header, 12, output);
}

// video reader: ivf -> vp8 -> yuv

VpxVideoReader *vpx_video_reader_open(const VpxVideoInfo *info) {
  VpxVideoReader *reader = malloc(sizeof(*reader));
  reader->info = *info;
  return reader;
}

void vpx_video_reader_close(VpxVideoReader *reader) {
  free(reader->buffer);
  free(reader);
}

void vpx_video_reader_read_frame(VpxVideoReader *reader, MFILE *input) {
  ivf_read_frame(input, &reader->buffer, &reader->frame_size,
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
  return writer;
}

int vpx_video_writer_write_frame(VpxVideoWriter *writer, MFILE *output,
  const uint8_t *buffer, size_t size, int64_t pts) {
  ivf_write_frame_header(output, pts, size);
  mfwrite(buffer, size, output);
  ++writer->frame_count;
  return 1;
}

void vpx_video_writer_close(VpxVideoWriter *writer) {
  vpx_img_free(&writer->img);
  free(writer);
}

int encode_frame(vpx_image_t *img, int frame_index, int flags,
  VpxVideoWriter *writer, MFILE *output) {

  int got_pkts = 0;
  vpx_codec_iter_t iter = NULL;
  const vpx_codec_cx_pkt_t *pkt = NULL;

  int res = vpx_codec_encode(&ctx_enc, img, frame_index, 1, flags, VPX_DL_REALTIME);
  if (res) die(("vpx_codec_encode failed: %d\n", (int)res));

  while ((pkt = vpx_codec_get_cx_data(&ctx_enc, &iter)) != NULL) {
    got_pkts = 1;

    if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
      if (!vpx_video_writer_write_frame(writer, output, pkt->data.frame.buf,
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
void vpx_js_decoder_run(uint8_t *rgba, uint8_t *ivfd, size_t ivfd_size) {
  MFILE input = mfopen(ivfd, ivfd_size);

  int width = reader->info.frame_width;
  int height = reader->info.frame_height;

  vpx_video_reader_read_frame(reader, &input);

  vpx_codec_iter_t iter = NULL;
  vpx_image_t *img = NULL;
  size_t frame_size = 0;
  const uint8_t *frame = NULL;

  frame = vpx_video_reader_get_frame(reader, &frame_size);
  // printf("Got an IVF frame: %d bytes\n", (int)frame_size);

  int res = vpx_codec_decode(&ctx_dec, frame, frame_size, NULL, 0);
  if (res) die(("vpx_codec_decode failed: %d\n", (int)res));

  img = vpx_codec_get_frame(&ctx_dec, &iter);

  // YUV frame can be 704x544 even if 640x480 is expected.
  // TODO: Scale the YUV image to the frame size first.
  // printf("Decoded a YUV frame: %dx%d.\n", img->w, img->h);
  int err = I420ToABGR(
    img->planes[0], img->stride[0],
    img->planes[1], img->stride[1],
    img->planes[2], img->stride[2],
    rgba, width * 4,
    width, height);

  if (err) die(("I420ToABGR failed: %d\n", err));
}

// ivfd: the output delta frame (or key frame) size = bitrate / fps.
// rgba = malloc(width*height*4);
EMSCRIPTEN_KEEPALIVE
size_t vpx_js_encoder_run(uint8_t *rgba, uint8_t *ivfd, size_t ivfd_size) {
  MFILE output = mfopen(ivfd, ivfd_size);
  vpx_image_t *img = &writer->img;

  int width = writer->info.frame_width;
  int height = writer->info.frame_height;

  int res = ABGRToI420(
    rgba, width * 4,
    img->planes[0], img->stride[0],
    img->planes[1], img->stride[1],
    img->planes[2], img->stride[2],
    width, height);

  if (res) die(("ABGRToI420 failed: %d\n", res));
  encode_frame(img, writer->frame_count++, 0, writer, &output);
  // while (encode_frame(NULL, -1, 0, writer, &output)) {}
  return output.pos + 1;
}
