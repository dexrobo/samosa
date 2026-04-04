#include "dex/infrastructure/video_monitor/fmp4_muxer.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace dex::video_monitor {
namespace {

// --- ISO BMFF box writing helpers ---

void WriteU8(std::vector<uint8_t>& buf, uint8_t val) { buf.push_back(val); }

void WriteU16BE(std::vector<uint8_t>& buf, uint16_t val) {
  buf.push_back(static_cast<uint8_t>(val >> 8));
  buf.push_back(static_cast<uint8_t>(val));
}

void WriteU32BE(std::vector<uint8_t>& buf, uint32_t val) {
  buf.push_back(static_cast<uint8_t>(val >> 24));
  buf.push_back(static_cast<uint8_t>(val >> 16));
  buf.push_back(static_cast<uint8_t>(val >> 8));
  buf.push_back(static_cast<uint8_t>(val));
}

void WriteU64BE(std::vector<uint8_t>& buf, uint64_t val) {
  WriteU32BE(buf, static_cast<uint32_t>(val >> 32));
  WriteU32BE(buf, static_cast<uint32_t>(val));
}

void WriteBytes(std::vector<uint8_t>& buf, const void* data, size_t len) {
  const auto* ptr = static_cast<const uint8_t*>(data);
  buf.insert(buf.end(), ptr, ptr + len);
}

// Write a 4CC type code.
void WriteType(std::vector<uint8_t>& buf, const char type[4]) { WriteBytes(buf, type, 4); }

// Start a box: write placeholder size + type. Returns the offset of the size field.
size_t BeginBox(std::vector<uint8_t>& buf, const char type[4]) {
  size_t offset = buf.size();
  WriteU32BE(buf, 0);  // Placeholder — patched by EndBox.
  WriteType(buf, type);
  return offset;
}

// Finish a box: patch the size field at the given offset.
void EndBox(std::vector<uint8_t>& buf, size_t offset) {
  uint32_t size = static_cast<uint32_t>(buf.size() - offset);
  buf[offset + 0] = static_cast<uint8_t>(size >> 24);
  buf[offset + 1] = static_cast<uint8_t>(size >> 16);
  buf[offset + 2] = static_cast<uint8_t>(size >> 8);
  buf[offset + 3] = static_cast<uint8_t>(size);
}

// Start a full box (box with version and flags).
size_t BeginFullBox(std::vector<uint8_t>& buf, const char type[4], uint8_t version, uint32_t flags) {
  size_t offset = BeginBox(buf, type);
  WriteU8(buf, version);
  // Flags: 3 bytes.
  WriteU8(buf, static_cast<uint8_t>((flags >> 16) & 0xFF));
  WriteU8(buf, static_cast<uint8_t>((flags >> 8) & 0xFF));
  WriteU8(buf, static_cast<uint8_t>(flags & 0xFF));
  return offset;
}

// Find the next Annex B start code (00 00 01 or 00 00 00 01) starting at `pos`.
// Returns the position of the first byte of the start code, or data_size if not found.
size_t FindStartCode(const uint8_t* data, size_t data_size, size_t pos) {
  for (size_t i = pos; i + 2 < data_size; ++i) {
    if (data[i] == 0 && data[i + 1] == 0) {
      if (data[i + 2] == 1) return i;
      if (i + 3 < data_size && data[i + 2] == 0 && data[i + 3] == 1) return i;
    }
  }
  return data_size;
}

// Return the length of the start code at position `pos` (3 or 4 bytes).
size_t StartCodeLen(const uint8_t* data, size_t pos) {
  // 00 00 00 01 → 4, 00 00 01 → 3.
  if (data[pos + 2] == 0) return 4;
  return 3;
}

}  // namespace

// --- FMP4Muxer implementation ---

FMP4Muxer::FMP4Muxer(const TrackParams& params) : params_(params) {}

std::vector<uint8_t> FMP4Muxer::GetInitSegment() const {
  std::vector<uint8_t> buf;
  buf.reserve(512);

  // --- ftyp box ---
  auto ftyp = BeginBox(buf, "ftyp");
  WriteType(buf, "isom");  // Major brand.
  WriteU32BE(buf, 0x200);  // Minor version.
  WriteType(buf, "isom");  // Compatible brands.
  WriteType(buf, "iso6");
  WriteType(buf, "mp41");
  WriteType(buf, "msdh");
  WriteType(buf, "msix");
  EndBox(buf, ftyp);

  // --- moov box ---
  auto moov = BeginBox(buf, "moov");

  // mvhd (movie header, version 0)
  auto mvhd = BeginFullBox(buf, "mvhd", 0, 0);
  WriteU32BE(buf, 0);                            // creation_time
  WriteU32BE(buf, 0);                            // modification_time
  WriteU32BE(buf, params_.timescale);            // timescale
  WriteU32BE(buf, 0);                            // duration (unknown for live)
  WriteU32BE(buf, 0x00010000);                   // rate (1.0 fixed-point 16.16)
  WriteU16BE(buf, 0x0100);                       // volume (1.0 fixed-point 8.8)
  for (int i = 0; i < 10; ++i) WriteU8(buf, 0);  // reserved
  // Identity matrix (9 x uint32).
  const uint32_t identity_matrix[] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
  for (auto m : identity_matrix) WriteU32BE(buf, m);
  for (int i = 0; i < 6; ++i) WriteU32BE(buf, 0);  // pre_defined
  WriteU32BE(buf, 2);                              // next_track_ID
  EndBox(buf, mvhd);

  // trak
  auto trak = BeginBox(buf, "trak");

  // tkhd (track header, version 0, flags=3: track_enabled | track_in_movie)
  auto tkhd = BeginFullBox(buf, "tkhd", 0, 3);
  WriteU32BE(buf, 0);  // creation_time
  WriteU32BE(buf, 0);  // modification_time
  WriteU32BE(buf, 1);  // track_ID
  WriteU32BE(buf, 0);  // reserved
  WriteU32BE(buf, 0);  // duration (unknown for live)
  WriteU32BE(buf, 0);  // reserved
  WriteU32BE(buf, 0);  // reserved
  WriteU16BE(buf, 0);  // layer
  WriteU16BE(buf, 0);  // alternate_group
  WriteU16BE(buf, 0);  // volume (0 for video)
  WriteU16BE(buf, 0);  // reserved
  for (auto m : identity_matrix) WriteU32BE(buf, m);
  WriteU32BE(buf, params_.width << 16);   // width (fixed-point 16.16)
  WriteU32BE(buf, params_.height << 16);  // height (fixed-point 16.16)
  EndBox(buf, tkhd);

  // mdia
  auto mdia = BeginBox(buf, "mdia");

  // mdhd (media header, version 0)
  auto mdhd = BeginFullBox(buf, "mdhd", 0, 0);
  WriteU32BE(buf, 0);                  // creation_time
  WriteU32BE(buf, 0);                  // modification_time
  WriteU32BE(buf, params_.timescale);  // timescale
  WriteU32BE(buf, 0);                  // duration
  WriteU32BE(buf, 0x55C40000);         // language (undetermined) + pre_defined
  EndBox(buf, mdhd);

  // hdlr (handler reference)
  auto hdlr = BeginFullBox(buf, "hdlr", 0, 0);
  WriteU32BE(buf, 0);                              // pre_defined
  WriteType(buf, "vide");                          // handler_type
  for (int i = 0; i < 3; ++i) WriteU32BE(buf, 0);  // reserved
  WriteBytes(buf, "VideoHandler", 13);             // name (null-terminated)
  EndBox(buf, hdlr);

  // minf
  auto minf = BeginBox(buf, "minf");

  // vmhd (video media header, flags=1)
  auto vmhd = BeginFullBox(buf, "vmhd", 0, 1);
  WriteU16BE(buf, 0);                              // graphicsmode
  for (int i = 0; i < 3; ++i) WriteU16BE(buf, 0);  // opcolor
  EndBox(buf, vmhd);

  // dinf + dref
  auto dinf = BeginBox(buf, "dinf");
  auto dref = BeginFullBox(buf, "dref", 0, 0);
  WriteU32BE(buf, 1);                          // entry_count
  auto url = BeginFullBox(buf, "url ", 0, 1);  // flags=1: data is in this file
  EndBox(buf, url);
  EndBox(buf, dref);
  EndBox(buf, dinf);

  // stbl
  auto stbl = BeginBox(buf, "stbl");

  // stsd (sample description)
  auto stsd = BeginFullBox(buf, "stsd", 0, 0);
  WriteU32BE(buf, 1);  // entry_count

  // avc1 sample entry
  auto avc1 = BeginBox(buf, "avc1");
  for (int i = 0; i < 6; ++i) WriteU8(buf, 0);     // reserved
  WriteU16BE(buf, 1);                              // data_reference_index
  WriteU16BE(buf, 0);                              // pre_defined
  WriteU16BE(buf, 0);                              // reserved
  for (int i = 0; i < 3; ++i) WriteU32BE(buf, 0);  // pre_defined
  WriteU16BE(buf, static_cast<uint16_t>(params_.width));
  WriteU16BE(buf, static_cast<uint16_t>(params_.height));
  WriteU32BE(buf, 0x00480000);                   // horizresolution (72 dpi)
  WriteU32BE(buf, 0x00480000);                   // vertresolution (72 dpi)
  WriteU32BE(buf, 0);                            // reserved
  WriteU16BE(buf, 1);                            // frame_count
  for (int i = 0; i < 32; ++i) WriteU8(buf, 0);  // compressorname
  WriteU16BE(buf, 0x0018);                       // depth (24-bit)
  WriteU16BE(buf, 0xFFFF);                       // pre_defined = -1

  // avcC (AVC Decoder Configuration Record)
  auto avcc = BeginBox(buf, "avcC");
  WriteU8(buf, 1);                                            // configurationVersion
  WriteU8(buf, params_.sps.empty() ? 0x42 : params_.sps[1]);  // AVCProfileIndication
  WriteU8(buf, params_.sps.empty() ? 0x00 : params_.sps[2]);  // profile_compatibility
  WriteU8(buf, params_.sps.empty() ? 0x0A : params_.sps[3]);  // AVCLevelIndication
  WriteU8(buf, 0xFF);                                         // lengthSizeMinusOne = 3 (4-byte NAL lengths)

  // SPS
  WriteU8(buf, 0xE1);  // numOfSequenceParameterSets = 1 (0xE0 | 1)
  WriteU16BE(buf, static_cast<uint16_t>(params_.sps.size()));
  WriteBytes(buf, params_.sps.data(), params_.sps.size());

  // PPS
  WriteU8(buf, 1);  // numOfPictureParameterSets
  WriteU16BE(buf, static_cast<uint16_t>(params_.pps.size()));
  WriteBytes(buf, params_.pps.data(), params_.pps.size());
  EndBox(buf, avcc);

  EndBox(buf, avc1);
  EndBox(buf, stsd);

  // Empty required boxes for fragmented MP4.
  auto stts = BeginFullBox(buf, "stts", 0, 0);
  WriteU32BE(buf, 0);  // entry_count
  EndBox(buf, stts);

  auto stsc = BeginFullBox(buf, "stsc", 0, 0);
  WriteU32BE(buf, 0);
  EndBox(buf, stsc);

  auto stsz = BeginFullBox(buf, "stsz", 0, 0);
  WriteU32BE(buf, 0);  // sample_size
  WriteU32BE(buf, 0);  // sample_count
  EndBox(buf, stsz);

  auto stco = BeginFullBox(buf, "stco", 0, 0);
  WriteU32BE(buf, 0);
  EndBox(buf, stco);

  EndBox(buf, stbl);
  EndBox(buf, minf);
  EndBox(buf, mdia);
  EndBox(buf, trak);

  // mvex (movie extends — signals fragmented MP4)
  auto mvex = BeginBox(buf, "mvex");
  auto trex = BeginFullBox(buf, "trex", 0, 0);
  WriteU32BE(buf, 1);  // track_ID
  WriteU32BE(buf, 1);  // default_sample_description_index
  WriteU32BE(buf, 0);  // default_sample_duration
  WriteU32BE(buf, 0);  // default_sample_size
  WriteU32BE(buf, 0);  // default_sample_flags
  EndBox(buf, trex);
  EndBox(buf, mvex);

  EndBox(buf, moov);

  return buf;
}

std::vector<uint8_t> FMP4Muxer::MuxFragment(const std::vector<uint8_t>& annex_b_nals, uint64_t decode_time,
                                            uint32_t duration, bool is_idr) {
  // Convert Annex B to length-prefixed for MP4.
  auto mp4_nals = AnnexBToLengthPrefixed(annex_b_nals);

  std::vector<uint8_t> buf;
  buf.reserve(mp4_nals.size() + 128);

  ++sequence_number_;

  // --- moof box ---
  auto moof = BeginBox(buf, "moof");

  // mfhd (movie fragment header)
  auto mfhd = BeginFullBox(buf, "mfhd", 0, 0);
  WriteU32BE(buf, sequence_number_);
  EndBox(buf, mfhd);

  // traf (track fragment)
  auto traf = BeginBox(buf, "traf");

  // tfhd (track fragment header)
  // flags: 0x020000 = default-base-is-moof
  auto tfhd = BeginFullBox(buf, "tfhd", 0, 0x020000);
  WriteU32BE(buf, 1);  // track_ID
  EndBox(buf, tfhd);

  // tfdt (track fragment decode time, version 1 for 64-bit time)
  auto tfdt = BeginFullBox(buf, "tfdt", 1, 0);
  WriteU64BE(buf, decode_time);
  EndBox(buf, tfdt);

  // trun (track fragment run)
  // flags: 0x000001 = data-offset-present
  //        0x000100 = sample-duration-present
  //        0x000200 = sample-size-present
  //        0x000400 = sample-flags-present
  const uint32_t trun_flags = 0x000001 | 0x000100 | 0x000200 | 0x000400;
  auto trun = BeginFullBox(buf, "trun", 0, trun_flags);
  WriteU32BE(buf, 1);  // sample_count = 1

  // data_offset: distance from moof start to mdat payload.
  // We'll patch this after we know the moof size.
  size_t data_offset_pos = buf.size();
  WriteU32BE(buf, 0);  // Placeholder for data_offset.

  // Sample entry.
  WriteU32BE(buf, duration);
  WriteU32BE(buf, static_cast<uint32_t>(mp4_nals.size()));

  // sample_flags:
  // For IDR:   0x02000000 (sample_depends_on=2: does not depend on others)
  // For non-IDR: 0x01010000 (sample_depends_on=1: depends on others, is_non_sync)
  uint32_t sample_flags = is_idr ? 0x02000000u : 0x01010000u;
  WriteU32BE(buf, sample_flags);

  EndBox(buf, trun);
  EndBox(buf, traf);
  EndBox(buf, moof);

  // Patch data_offset: from start of moof to start of mdat payload (after mdat header).
  uint32_t mdat_header_size = 8;  // 4 bytes size + 4 bytes "mdat".
  uint32_t data_offset = static_cast<uint32_t>(buf.size() - moof + mdat_header_size);
  buf[data_offset_pos + 0] = static_cast<uint8_t>(data_offset >> 24);
  buf[data_offset_pos + 1] = static_cast<uint8_t>(data_offset >> 16);
  buf[data_offset_pos + 2] = static_cast<uint8_t>(data_offset >> 8);
  buf[data_offset_pos + 3] = static_cast<uint8_t>(data_offset);

  // --- mdat box ---
  auto mdat = BeginBox(buf, "mdat");
  WriteBytes(buf, mp4_nals.data(), mp4_nals.size());
  EndBox(buf, mdat);

  return buf;
}

// --- Utility functions ---

bool ExtractSPSPPS(const std::vector<uint8_t>& annex_b, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) {
  sps.clear();
  pps.clear();

  const uint8_t* data = annex_b.data();
  size_t size = annex_b.size();

  size_t pos = 0;
  while (pos < size) {
    size_t sc = FindStartCode(data, size, pos);
    if (sc >= size) break;

    size_t sc_len = StartCodeLen(data, sc);
    size_t nalu_start = sc + sc_len;
    size_t next_sc = FindStartCode(data, size, nalu_start);
    size_t nalu_len = next_sc - nalu_start;

    if (nalu_len > 0) {
      uint8_t nalu_type = data[nalu_start] & 0x1F;
      if (nalu_type == 7 && sps.empty()) {
        sps.assign(data + nalu_start, data + nalu_start + nalu_len);
      } else if (nalu_type == 8 && pps.empty()) {
        pps.assign(data + nalu_start, data + nalu_start + nalu_len);
      }
    }

    pos = next_sc;
  }

  return !sps.empty() && !pps.empty();
}

std::vector<uint8_t> AnnexBToLengthPrefixed(const std::vector<uint8_t>& annex_b) {
  std::vector<uint8_t> result;
  result.reserve(annex_b.size());

  const uint8_t* data = annex_b.data();
  size_t size = annex_b.size();

  size_t pos = 0;
  while (pos < size) {
    size_t sc = FindStartCode(data, size, pos);
    if (sc >= size) break;

    size_t sc_len = StartCodeLen(data, sc);
    size_t nalu_start = sc + sc_len;
    size_t next_sc = FindStartCode(data, size, nalu_start);
    size_t nalu_len = next_sc - nalu_start;

    if (nalu_len > 0) {
      // 4-byte big-endian length prefix.
      auto len32 = static_cast<uint32_t>(nalu_len);
      WriteU32BE(result, len32);
      WriteBytes(result, data + nalu_start, nalu_len);
    }

    pos = next_sc;
  }

  return result;
}

}  // namespace dex::video_monitor
