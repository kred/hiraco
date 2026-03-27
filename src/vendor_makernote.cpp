#include "vendor_makernote.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

// Vendor MakerNote header variants.
// Classic: "CLASSIC_VENDOR\x00II\x03\x00" (12 bytes, IFD at +12)
// MODERN_VENDOR: "MODERN_VENDOR\x00\x00\x00II\x04\x00" (16 bytes, IFD at +16)
static constexpr char kVendorHeaderClassic[] = "OLYMPUS\x00II";
static constexpr size_t kVendorHeaderClassicLen = 10;
static constexpr char kVendorHeaderModern[] = "OM SYSTEM\x00\x00\x00II";
static constexpr size_t kVendorHeaderModernLen = 14;

// TIFF/EXIF tag IDs
static constexpr uint16_t kTagExifIFD = 0x8769;
static constexpr uint16_t kTagMakerNote = 0x927C;

// Vendor MakerNote sub-IFD pointer tags (within the root MakerNote IFD)
// Classic CLASSIC_VENDOR format:
static constexpr uint16_t kTagEquipment = 0x0001;
static constexpr uint16_t kTagCameraSettings = 0x0002;
static constexpr uint16_t kTagRawDevelopment = 0x0003;
static constexpr uint16_t kTagImageProcessing = 0x0004;
static constexpr uint16_t kTagFocusInfo = 0x0005;
static constexpr uint16_t kTagRawInfo = 0x0007;

// MODERN_VENDOR format sub-IFD pointer tags (type=13 IFD):
static constexpr uint16_t kTagOmsEquipment = 0x2010;
static constexpr uint16_t kTagOmsCameraSettings = 0x2020;
static constexpr uint16_t kTagOmsRawDevelopment = 0x2030;
static constexpr uint16_t kTagOmsImageProcessing = 0x2040;
static constexpr uint16_t kTagOmsFocusInfo = 0x2050;

// Classic ImageProcessing sub-IFD tags
static constexpr uint16_t kTagStackedImage = 0x0015;

// MODERN_VENDOR ImageProcessing sub-IFD tags
static constexpr uint16_t kTagOmsStackedImage = 0x0804;

// Classic RawInfo sub-IFD tag IDs for UnknownBlock entries.
static constexpr uint16_t kTagUnknownBlock1 = 0x0200;
static constexpr uint16_t kTagUnknownBlock2 = 0x0201;
static constexpr uint16_t kTagUnknownBlock3 = 0x0202;
static constexpr uint16_t kTagUnknownBlock4 = 0x0203;

// MODERN_VENDOR ImageProcessing sub-IFD tag IDs for UnknownBlock entries.
// In the MODERN_VENDOR format these live in ImageProcessing rather than RawInfo.
static constexpr uint16_t kTagOmsUnknownBlock1 = 0x0635;
static constexpr uint16_t kTagOmsUnknownBlock2 = 0x0636;
static constexpr uint16_t kTagOmsUnknownBlock3 = 0x1103;
static constexpr uint16_t kTagOmsUnknownBlock4 = 0x1104;

// TIFF type IDs
static constexpr uint16_t kTypeByte = 1;
static constexpr uint16_t kTypeAscii = 2;
static constexpr uint16_t kTypeShort = 3;
static constexpr uint16_t kTypeLong = 4;
static constexpr uint16_t kTypeRational = 5;
static constexpr uint16_t kTypeSByte = 6;
static constexpr uint16_t kTypeUndefined = 7;
static constexpr uint16_t kTypeSShort = 8;
static constexpr uint16_t kTypeSLong = 9;
static constexpr uint16_t kTypeSRational = 10;

size_t TypeSize(uint16_t type) {
  switch (type) {
    case kTypeByte:
    case kTypeAscii:
    case kTypeSByte:
    case kTypeUndefined:
      return 1;
    case kTypeShort:
    case kTypeSShort:
      return 2;
    case kTypeLong:
    case kTypeSLong:
      return 4;
    case kTypeRational:
    case kTypeSRational:
      return 8;
    default:
      return 1;
  }
}

class TiffReader {
 public:
  TiffReader(const std::vector<uint8_t>& data, bool little_endian)
      : data_(data), little_endian_(little_endian) {}

  bool Valid(size_t offset, size_t length) const {
    return offset + length <= data_.size() && offset + length >= offset;
  }

  uint8_t ReadU8(size_t offset) const {
    return data_[offset];
  }

  uint16_t ReadU16(size_t offset) const {
    if (little_endian_) {
      return static_cast<uint16_t>(data_[offset]) |
             (static_cast<uint16_t>(data_[offset + 1]) << 8);
    }
    return (static_cast<uint16_t>(data_[offset]) << 8) |
           static_cast<uint16_t>(data_[offset + 1]);
  }

  uint32_t ReadU32(size_t offset) const {
    if (little_endian_) {
      return static_cast<uint32_t>(data_[offset]) |
             (static_cast<uint32_t>(data_[offset + 1]) << 8) |
             (static_cast<uint32_t>(data_[offset + 2]) << 16) |
             (static_cast<uint32_t>(data_[offset + 3]) << 24);
    }
    return (static_cast<uint32_t>(data_[offset]) << 24) |
           (static_cast<uint32_t>(data_[offset + 1]) << 16) |
           (static_cast<uint32_t>(data_[offset + 2]) << 8) |
           static_cast<uint32_t>(data_[offset + 3]);
  }

  size_t Size() const { return data_.size(); }
  const uint8_t* Data() const { return data_.data(); }
  bool IsLittleEndian() const { return little_endian_; }

 private:
  const std::vector<uint8_t>& data_;
  bool little_endian_;
};

struct IfdEntry {
  uint16_t tag = 0;
  uint16_t type = 0;
  uint32_t count = 0;
  uint32_t value_offset = 0;  // offset into file data (or inline value)
  bool inline_value = false;
};

// Parse a single IFD at the given offset within the TIFF data.
// base_offset is added to all offsets read from the IFD entries (used
// for MakerNote sub-IFDs where offsets are relative to the MakerNote start).
bool ParseIfd(const TiffReader& reader,
              size_t ifd_offset,
              size_t base_offset,
              std::vector<IfdEntry>* entries,
              uint32_t* next_ifd_offset) {
  if (!reader.Valid(ifd_offset, 2)) return false;

  const uint16_t entry_count = reader.ReadU16(ifd_offset);
  if (entry_count > 4096) return false;  // sanity

  const size_t entries_start = ifd_offset + 2;
  const size_t entries_end = entries_start + 12 * static_cast<size_t>(entry_count);
  if (!reader.Valid(entries_start, 12 * static_cast<size_t>(entry_count))) return false;

  entries->resize(entry_count);
  for (uint16_t i = 0; i < entry_count; ++i) {
    const size_t pos = entries_start + 12 * static_cast<size_t>(i);
    IfdEntry& e = (*entries)[i];
    e.tag = reader.ReadU16(pos);
    e.type = reader.ReadU16(pos + 2);
    e.count = reader.ReadU32(pos + 4);

    const size_t value_size = TypeSize(e.type) * e.count;
    if (value_size <= 4) {
      // Value stored inline in the 4-byte value/offset field.
      e.value_offset = static_cast<uint32_t>(pos + 8);
      e.inline_value = true;
    } else {
      const uint32_t raw_offset = reader.ReadU32(pos + 8);
      e.value_offset = static_cast<uint32_t>(base_offset) + raw_offset;
      e.inline_value = false;
    }
  }

  if (next_ifd_offset != nullptr) {
    if (reader.Valid(entries_end, 4)) {
      *next_ifd_offset = reader.ReadU32(entries_end);
    } else {
      *next_ifd_offset = 0;
    }
  }

  return true;
}

const IfdEntry* FindEntry(const std::vector<IfdEntry>& entries, uint16_t tag) {
  for (const auto& e : entries) {
    if (e.tag == tag) return &e;
  }
  return nullptr;
}

uint32_t EntryU32(const TiffReader& reader, const IfdEntry& entry) {
  if (entry.type == kTypeShort) {
    return reader.ReadU16(entry.value_offset);
  }
  return reader.ReadU32(entry.value_offset);
}

std::vector<uint8_t> EntryBinaryPayload(const TiffReader& reader, const IfdEntry& entry) {
  const size_t total = TypeSize(entry.type) * entry.count;
  if (!reader.Valid(entry.value_offset, total)) return {};
  return std::vector<uint8_t>(
      reader.Data() + entry.value_offset,
      reader.Data() + entry.value_offset + total);
}

// Read a SHORT or LONG array from an IFD entry and return as vector<uint32_t>.
std::vector<uint32_t> EntryU32Array(const TiffReader& reader, const IfdEntry& entry) {
  std::vector<uint32_t> result;
  result.reserve(entry.count);
  for (uint32_t i = 0; i < entry.count; ++i) {
    const size_t pos = entry.value_offset + i * TypeSize(entry.type);
    if (!reader.Valid(pos, TypeSize(entry.type))) break;
    if (entry.type == kTypeShort || entry.type == kTypeSShort) {
      result.push_back(reader.ReadU16(pos));
    } else {
      result.push_back(reader.ReadU32(pos));
    }
  }
  return result;
}

// Read an ASCII string from an IFD entry.
std::string EntryString(const TiffReader& reader, const IfdEntry& entry) {
  const size_t total = TypeSize(entry.type) * entry.count;
  if (!reader.Valid(entry.value_offset, total)) return {};
  std::string s(reinterpret_cast<const char*>(reader.Data() + entry.value_offset), total);
  // Trim trailing NUL bytes and spaces.
  while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) {
    s.pop_back();
  }
  return s;
}

// Decode the StackedImage numeric array into a human-readable label
// matching ExifTool's decoding (e.g. "Hand-held high resolution (11 12)").
std::string DecodeStackedImageLabel(const std::vector<uint32_t>& values) {
  if (values.size() < 2) {
    if (values.size() == 1 && values[0] == 0) return "No";
    return {};
  }

  std::string mode;
  switch (values[0]) {
    case 0:  return "No";
    case 1:  mode = "HDR"; break;
    case 3:  mode = "Focus-stacked"; break;
    case 5:  mode = "ND filter"; break;
    case 9:  mode = "Focus-stacked + ND filter"; break;
    case 11: mode = "Hand-held high resolution"; break;
    case 13: mode = "Starry sky"; break;
    default: mode = "Unknown(" + std::to_string(values[0]) + ")"; break;
  }

  std::string label = mode + " (";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) label += " ";
    label += std::to_string(values[i]);
  }
  label += ")";
  return label;
}

}  // namespace

VendorMakerNoteResult ReadVendorMakerNote(const std::string& path) {
  VendorMakerNoteResult result;

  // Read the entire file into memory.
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    result.error = "cannot open file: " + path;
    return result;
  }

  const auto file_size = file.tellg();
  if (file_size <= 0 || file_size > 512 * 1024 * 1024) {
    result.error = "file size out of range";
    return result;
  }

  std::vector<uint8_t> data(static_cast<size_t>(file_size));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(data.data()), file_size);
  if (!file.good()) {
    result.error = "failed to read file";
    return result;
  }

  // Parse TIFF header.
  if (data.size() < 8) {
    result.error = "file too small for TIFF header";
    return result;
  }

  bool little_endian;
  if (data[0] == 'I' && data[1] == 'I') {
    little_endian = true;
  } else if (data[0] == 'M' && data[1] == 'M') {
    little_endian = false;
  } else {
    result.error = "not a TIFF file";
    return result;
  }

  TiffReader reader(data, little_endian);

  const uint16_t magic = reader.ReadU16(2);
  if (magic != 42) {
    // ORF files use a variant magic (0x4F52 = "OR" for Vendor raw).
    // Accept common Vendor ORF magics as well as standard TIFF.
    if (magic != 0x4F52 && magic != 0x5352) {
      result.error = "unsupported TIFF magic: " + std::to_string(magic);
      return result;
    }
  }

  const uint32_t ifd0_offset = reader.ReadU32(4);
  if (!reader.Valid(ifd0_offset, 2)) {
    result.error = "IFD0 offset out of range";
    return result;
  }

  // Parse IFD0 to find EXIF IFD pointer.
  std::vector<IfdEntry> ifd0_entries;
  uint32_t next_ifd = 0;
  if (!ParseIfd(reader, ifd0_offset, 0, &ifd0_entries, &next_ifd)) {
    result.error = "failed to parse IFD0";
    return result;
  }

  // Read TIFF Make (tag 0x010F) and Model (tag 0x0110) from IFD0.
  {
    const IfdEntry* make_entry = FindEntry(ifd0_entries, 0x010F);
    if (make_entry != nullptr) {
      result.tiff_make = EntryString(reader, *make_entry);
    }
    const IfdEntry* model_entry = FindEntry(ifd0_entries, 0x0110);
    if (model_entry != nullptr) {
      result.tiff_model = EntryString(reader, *model_entry);
    }
  }

  const IfdEntry* exif_entry = FindEntry(ifd0_entries, kTagExifIFD);
  if (exif_entry == nullptr) {
    result.error = "no EXIF IFD pointer in IFD0";
    return result;
  }

  const uint32_t exif_ifd_offset = EntryU32(reader, *exif_entry);
  if (!reader.Valid(exif_ifd_offset, 2)) {
    result.error = "EXIF IFD offset out of range";
    return result;
  }

  // Parse EXIF IFD to find MakerNote.
  std::vector<IfdEntry> exif_entries;
  if (!ParseIfd(reader, exif_ifd_offset, 0, &exif_entries, nullptr)) {
    result.error = "failed to parse EXIF IFD";
    return result;
  }

  const IfdEntry* makernote_entry = FindEntry(exif_entries, kTagMakerNote);
  if (makernote_entry == nullptr) {
    result.error = "no MakerNote tag in EXIF IFD";
    return result;
  }

  // Verify Vendor or MODERN_VENDOR MakerNote header.
  const size_t mn_offset = makernote_entry->value_offset;
  const size_t mn_size = TypeSize(makernote_entry->type) * makernote_entry->count;

  size_t mn_ifd_start = 0;  // offset from mn_offset to the first IFD
  if (reader.Valid(mn_offset, kVendorHeaderModernLen + 2) &&
      std::memcmp(reader.Data() + mn_offset, kVendorHeaderModern, kVendorHeaderModernLen) == 0) {
    // MODERN_VENDOR header: 12 bytes name + 2 byte order + 2 version = 16
    mn_ifd_start = 16;
  } else if (reader.Valid(mn_offset, kVendorHeaderClassicLen + 2) &&
             std::memcmp(reader.Data() + mn_offset, kVendorHeaderClassic, kVendorHeaderClassicLen) == 0) {
    // Classic CLASSIC_VENDOR header: 8 bytes name + 2 byte order + 2 version = 12
    mn_ifd_start = 12;
  } else {
    result.error = "MakerNote does not have Vendor/MODERN_VENDOR header";
    return result;
  }

  // MakerNote IFD offsets are relative to mn_offset.
  const size_t mn_base = mn_offset;

  const size_t mn_ifd_offset = mn_offset + mn_ifd_start;
  if (!reader.Valid(mn_ifd_offset, 2)) {
    result.error = "MakerNote IFD offset out of range";
    return result;
  }

  std::vector<IfdEntry> mn_entries;
  if (!ParseIfd(reader, mn_ifd_offset, mn_base, &mn_entries, nullptr)) {
    result.error = "failed to parse MakerNote IFD";
    return result;
  }

  // Determine format: classic CLASSIC_VENDOR vs MODERN_VENDOR.
  const bool is_modern_vendor = (mn_ifd_start == 16);

  // Navigate to ImageProcessing sub-IFD.
  const uint16_t ip_tag = is_modern_vendor ? kTagOmsImageProcessing : kTagImageProcessing;
  const IfdEntry* img_processing_ptr = FindEntry(mn_entries, ip_tag);
  std::vector<IfdEntry> ip_entries;
  if (img_processing_ptr != nullptr) {
    const uint32_t ip_offset = static_cast<uint32_t>(mn_base) + EntryU32(reader, *img_processing_ptr);
    if (reader.Valid(ip_offset, 2)) {
      ParseIfd(reader, ip_offset, mn_base, &ip_entries, nullptr);
    }
  }

  // Read StackedImage.
  // Classic: tag 0x0015 in ImageProcessing sub-IFD.
  // MODERN_VENDOR: tag 0x0804 in CameraSettings sub-IFD (0x2020).
  if (is_modern_vendor) {
    const IfdEntry* cs_ptr = FindEntry(mn_entries, kTagOmsCameraSettings);
    if (cs_ptr != nullptr) {
      const uint32_t cs_offset = static_cast<uint32_t>(mn_base) + EntryU32(reader, *cs_ptr);
      if (reader.Valid(cs_offset, 2)) {
        std::vector<IfdEntry> cs_entries;
        if (ParseIfd(reader, cs_offset, mn_base, &cs_entries, nullptr)) {
          const IfdEntry* stacked = FindEntry(cs_entries, kTagOmsStackedImage);
          if (stacked != nullptr) {
            auto values = EntryU32Array(reader, *stacked);
            if (!values.empty()) {
              result.has_stacked_image = true;
              result.stacked_image_label = DecodeStackedImageLabel(values);
            }
          }
        }
      }
    }
  } else if (!ip_entries.empty()) {
    const IfdEntry* stacked = FindEntry(ip_entries, kTagStackedImage);
    if (stacked != nullptr) {
      auto values = EntryU32Array(reader, *stacked);
      if (!values.empty()) {
        result.has_stacked_image = true;
        result.stacked_image_label = DecodeStackedImageLabel(values);
      }
    }
  }

  // Read UnknownBlock1 and UnknownBlock3.
  // In classic format: from RawInfo sub-IFD (tag 0x0007), tags 0x0200/0x0202.
  // In MODERN_VENDOR format: from ImageProcessing sub-IFD (tag 0x2040), tags 0x0635/0x1103.
  if (is_modern_vendor) {
    // MODERN_VENDOR: UnknownBlocks are in ImageProcessing sub-IFD.
    if (!ip_entries.empty()) {
      const IfdEntry* b1 = FindEntry(ip_entries, kTagOmsUnknownBlock1);
      if (b1 != nullptr) {
        result.unknown_block_1 = EntryBinaryPayload(reader, *b1);
      }
      const IfdEntry* b3 = FindEntry(ip_entries, kTagOmsUnknownBlock3);
      if (b3 != nullptr) {
        result.unknown_block_3 = EntryBinaryPayload(reader, *b3);
      }
    }
  } else {
    // Classic: UnknownBlocks are in RawInfo sub-IFD.
    const IfdEntry* raw_info_ptr = FindEntry(mn_entries, kTagRawInfo);
    if (raw_info_ptr != nullptr) {
      const uint32_t ri_offset = static_cast<uint32_t>(mn_base) + EntryU32(reader, *raw_info_ptr);
      if (reader.Valid(ri_offset, 2)) {
        std::vector<IfdEntry> ri_entries;
        if (ParseIfd(reader, ri_offset, mn_base, &ri_entries, nullptr)) {
          const IfdEntry* b1 = FindEntry(ri_entries, kTagUnknownBlock1);
          if (b1 != nullptr) {
            result.unknown_block_1 = EntryBinaryPayload(reader, *b1);
          }
          const IfdEntry* b3 = FindEntry(ri_entries, kTagUnknownBlock3);
          if (b3 != nullptr) {
            result.unknown_block_3 = EntryBinaryPayload(reader, *b3);
          }
        }
      }
    }
  }

  // Extract working dimensions from UnknownBlock1 offset 23 (as uint32 LE).
  if (result.unknown_block_1.size() >= 24 * 4) {
    uint32_t dim_word;
    std::memcpy(&dim_word, result.unknown_block_1.data() + 23 * 4, 4);
    // UnknownBlock1 is always little-endian in Vendor ORFs.
    // If the file is big-endian, we would need to swap, but ORFs are always LE.
    const uint32_t h = (dim_word >> 16) & 0xFFFF;
    const uint32_t w = dim_word & 0xFFFF;
    if (w > 0 && h > 0) {
      result.has_working_geometry = true;
      result.working_width = w;
      result.working_height = h;
    }
  }

  result.ok = true;
  return result;
}
