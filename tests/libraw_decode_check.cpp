// libraw_decode_check.cpp  —  Decodes a RAW file with LibRaw and writes the
// processed image to a file.
//
// The WebAssembly build compiles LibRaw from source for wasm64 rather than
// using the system library, so the two builds do not execute the same
// machine code.  This program produces the pixels both builds derive from a
// RAW file so the difference can be measured instead of assumed.
//
// It mirrors the LibRaw call sequence that `dng_writer_bridge.cpp` uses to
// render an image, so a mismatch here is a mismatch in the conversion.
//
// Usage:
//   hiraco-libraw-check <input.ORF> <output.bin>
#include <libraw/libraw.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

struct DumpHeader {
  uint32_t width;
  uint32_t height;
  uint32_t colors;
  uint32_t bits;
  uint32_t black;
  uint32_t maximum;
  uint32_t flip;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <input.raw> <output.bin>\n", argv[0]);
    return 2;
  }

  LibRaw processor;
  int result = processor.open_file(argv[1]);
  if (result != LIBRAW_SUCCESS) {
    fprintf(stderr, "open_file failed: %s\n", libraw_strerror(result));
    return 1;
  }

  result = processor.unpack();
  if (result != LIBRAW_SUCCESS) {
    fprintf(stderr, "unpack failed: %s\n", libraw_strerror(result));
    return 1;
  }

  // The same settings `dng_writer_bridge.cpp` applies before processing.
  libraw_set_output_bps(&processor.imgdata, 16);
  libraw_set_output_color(&processor.imgdata, 4);
  libraw_set_no_auto_bright(&processor.imgdata, 0);
  libraw_set_gamma(&processor.imgdata, 0, 1.0);
  libraw_set_gamma(&processor.imgdata, 1, 1.0);
  processor.imgdata.params.use_camera_wb = 1;
  processor.imgdata.params.use_camera_matrix = 1;
  processor.imgdata.params.no_auto_scale = 0;
  processor.imgdata.params.user_flip = -1;
  processor.imgdata.params.user_qual = -1;
  processor.imgdata.params.half_size = 0;

  result = processor.dcraw_process();
  if (result != LIBRAW_SUCCESS) {
    fprintf(stderr, "dcraw_process failed: %s\n", libraw_strerror(result));
    return 1;
  }

  int memory_error = 0;
  libraw_processed_image_t* image = processor.dcraw_make_mem_image(&memory_error);
  if (image == nullptr) {
    fprintf(stderr, "dcraw_make_mem_image failed: %d\n", memory_error);
    return 1;
  }

  DumpHeader header;
  header.width = image->width;
  header.height = image->height;
  header.colors = image->colors;
  header.bits = image->bits;
  header.black = processor.imgdata.color.black;
  header.maximum = processor.imgdata.color.maximum;
  header.flip = processor.imgdata.sizes.flip;

  const size_t sample_count =
      static_cast<size_t>(image->width) * image->height * image->colors;
  std::vector<uint16_t> samples(sample_count);
  for (size_t index = 0; index < sample_count; ++index) {
    samples[index] = image->data[index];
  }

  FILE* handle = fopen(argv[2], "wb");
  if (handle == nullptr) {
    fprintf(stderr, "cannot open %s for writing\n", argv[2]);
    return 1;
  }
  fwrite(&header, sizeof(header), 1, handle);
  fwrite(samples.data(), sizeof(uint16_t), samples.size(), handle);
  fclose(handle);

  printf("ok: %ux%u colors=%u bits=%u black=%u max=%u flip=%d samples=%zu\n",
         header.width, header.height, header.colors, header.bits,
         header.black, header.maximum, header.flip, sample_count);
  return 0;
}
