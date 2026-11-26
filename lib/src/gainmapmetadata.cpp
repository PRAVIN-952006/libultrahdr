/*
 * Copyright 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
 * https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
 * <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
 * option. This file may not be copied, modified, or distributed
 * except according to those terms.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <sstream>
#include <string_view>

#include "ultrahdr/gainmapmath.h"
#include "ultrahdr/gainmapmetadata.h"
#include "ultrahdr/ultrahdrcommon.h"

namespace ultrahdr {

static uhdr_error_info_t makeError(uhdr_codec_err_t code, std::string_view message) {
  uhdr_error_info_t status{};
  status.error_code = code;
  status.has_detail = 1;
  const std::string_view clipped = message.substr(0, sizeof(status.detail) - 1);
  std::copy(clipped.begin(), clipped.end(), std::begin(status.detail));
  return status;
}

void streamWriteU8(std::vector<uint8_t> &data, uint8_t value) { data.push_back(value); }

void streamWriteU16(std::vector<uint8_t> &data, uint16_t value) {
  data.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
  data.push_back(static_cast<uint8_t>(value & 0xffu));
}

void streamWriteU32(std::vector<uint8_t> &data, uint32_t value) {
  data.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
  data.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
  data.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
  data.push_back(static_cast<uint8_t>(value & 0xffu));
}

void streamWriteS32(std::vector<uint8_t> &data, int32_t value) {
  streamWriteU32(data, static_cast<uint32_t>(value));
}

uhdr_error_info_t streamReadU8(const std::vector<uint8_t> &data, uint8_t &value, size_t &pos) {
  if (pos >= data.size()) {
    std::stringstream ss;
    ss << "attempting to read byte at position " << pos << " when the buffer size is "
       << data.size();
    return makeError(UHDR_CODEC_MEM_ERROR, ss.str());
  }
  value = data[pos++];
  return g_no_error;
}

uhdr_error_info_t streamReadU16(const std::vector<uint8_t> &data, uint16_t &value, size_t &pos) {
  if (pos > data.size() || data.size() - pos < 2) {
    std::stringstream ss;
    ss << "attempting to read 2 bytes from position " << pos << " when the buffer size is "
       << data.size();
    return makeError(UHDR_CODEC_MEM_ERROR, ss.str());
  }
  value = static_cast<uint16_t>((static_cast<uint16_t>(data[pos]) << 8) | data[pos + 1]);
  pos += 2;
  return g_no_error;
}

uhdr_error_info_t streamReadU32(const std::vector<uint8_t> &data, uint32_t &value, size_t &pos) {
  if (pos > data.size() || data.size() - pos < 4) {
    std::stringstream ss;
    ss << "attempting to read 4 bytes from position " << pos << " when the buffer size is "
       << data.size();
    return makeError(UHDR_CODEC_MEM_ERROR, ss.str());
  }
  value = (static_cast<uint32_t>(data[pos]) << 24) | (static_cast<uint32_t>(data[pos + 1]) << 16) |
          (static_cast<uint32_t>(data[pos + 2]) << 8) | static_cast<uint32_t>(data[pos + 3]);
  pos += 4;
  return g_no_error;
}

uhdr_error_info_t streamReadS32(const std::vector<uint8_t> &data, int32_t &value, size_t &pos) {
  uint32_t u = 0;
  UHDR_ERR_CHECK(streamReadU32(data, u, pos))
  value = static_cast<int32_t>(u);
  return g_no_error;
}

bool uhdr_gainmap_metadata_frac::allChannelsIdentical() const {
  return gainMapMinN[0] == gainMapMinN[1] && gainMapMinN[0] == gainMapMinN[2] &&
         gainMapMinD[0] == gainMapMinD[1] && gainMapMinD[0] == gainMapMinD[2] &&
         gainMapMaxN[0] == gainMapMaxN[1] && gainMapMaxN[0] == gainMapMaxN[2] &&
         gainMapMaxD[0] == gainMapMaxD[1] && gainMapMaxD[0] == gainMapMaxD[2] &&
         gainMapGammaN[0] == gainMapGammaN[1] && gainMapGammaN[0] == gainMapGammaN[2] &&
         gainMapGammaD[0] == gainMapGammaD[1] && gainMapGammaD[0] == gainMapGammaD[2] &&
         baseOffsetN[0] == baseOffsetN[1] && baseOffsetN[0] == baseOffsetN[2] &&
         baseOffsetD[0] == baseOffsetD[1] && baseOffsetD[0] == baseOffsetD[2] &&
         alternateOffsetN[0] == alternateOffsetN[1] && alternateOffsetN[0] == alternateOffsetN[2] &&
         alternateOffsetD[0] == alternateOffsetD[1] && alternateOffsetD[0] == alternateOffsetD[2];
}

uhdr_error_info_t uhdr_gainmap_metadata_frac::encodeGainmapMetadata(
    const uhdr_gainmap_metadata_frac *in_metadata, std::vector<uint8_t> &out_data) {
  if (in_metadata == nullptr) {
    return makeError(UHDR_CODEC_INVALID_PARAM,
                     "received nullptr for gain map metadata descriptor");
  }

  if (in_metadata->backwardDirection) {
    return makeError(UHDR_CODEC_UNSUPPORTED_FEATURE, "backward direction is not supported");
  }

  uint8_t flags = 0u;
  // Version 0 defines only the channel-count and base-color-space flags.
  const size_t channelCount = in_metadata->allChannelsIdentical() ? 1u : 3u;

  if (channelCount == 3) {
    flags |= kIsMultiChannelMask;
  }
  if (in_metadata->useBaseColorSpace) {
    flags |= kUseBaseColorSpaceMask;
  }

  // Validate that no denominator is zero before serialization
  if (in_metadata->baseHdrHeadroomD == 0 || in_metadata->alternateHdrHeadroomD == 0) {
    return makeError(UHDR_CODEC_INVALID_PARAM,
                     "gain map metadata headroom denominator cannot be 0");
  }
  for (size_t c = 0; c < channelCount; ++c) {
    if (in_metadata->gainMapMinD[c] == 0 || in_metadata->gainMapMaxD[c] == 0 ||
        in_metadata->gainMapGammaD[c] == 0 || in_metadata->baseOffsetD[c] == 0 ||
        in_metadata->alternateOffsetD[c] == 0) {
      std::stringstream ss;
      ss << "gain map metadata channel " << c << " denominator cannot be 0";
      return makeError(UHDR_CODEC_INVALID_PARAM, ss.str());
    }
  }

  // Nothing is appended to out_data until every field has been validated, so a
  // rejected descriptor leaves the caller's buffer untouched.
  const uint16_t min_version = 0, writer_version = 0;
  streamWriteU16(out_data, min_version);
  streamWriteU16(out_data, writer_version);

  // Per ISO/IEC 21496-1, the common-denominator bit is reserved and MUST be 0.
  // Always emit independent rational fraction pairs (N, D).
  streamWriteU8(out_data, flags);

  streamWriteU32(out_data, in_metadata->baseHdrHeadroomN);
  streamWriteU32(out_data, in_metadata->baseHdrHeadroomD);
  streamWriteU32(out_data, in_metadata->alternateHdrHeadroomN);
  streamWriteU32(out_data, in_metadata->alternateHdrHeadroomD);
  for (size_t c = 0; c < channelCount; ++c) {
    streamWriteS32(out_data, in_metadata->gainMapMinN[c]);
    streamWriteU32(out_data, in_metadata->gainMapMinD[c]);
    streamWriteS32(out_data, in_metadata->gainMapMaxN[c]);
    streamWriteU32(out_data, in_metadata->gainMapMaxD[c]);
    streamWriteU32(out_data, in_metadata->gainMapGammaN[c]);
    streamWriteU32(out_data, in_metadata->gainMapGammaD[c]);
    streamWriteS32(out_data, in_metadata->baseOffsetN[c]);
    streamWriteU32(out_data, in_metadata->baseOffsetD[c]);
    streamWriteS32(out_data, in_metadata->alternateOffsetN[c]);
    streamWriteU32(out_data, in_metadata->alternateOffsetD[c]);
  }

  return g_no_error;
}

uhdr_error_info_t uhdr_gainmap_metadata_frac::decodeGainmapMetadata(
    const std::vector<uint8_t> &in_data, uhdr_gainmap_metadata_frac *out_metadata) {
  if (out_metadata == nullptr) {
    return makeError(UHDR_CODEC_INVALID_PARAM,
                     "received nullptr for gain map metadata descriptor");
  }

  size_t pos = 0;
  uint16_t min_version = 0xffff;
  uint16_t writer_version = 0xffff;
  UHDR_ERR_CHECK(streamReadU16(in_data, min_version, pos))
  if (min_version != 0) {
    std::stringstream ss;
    ss << "received unexpected minimum version " << min_version << ", expected 0";
    return makeError(UHDR_CODEC_UNSUPPORTED_FEATURE, ss.str());
  }
  UHDR_ERR_CHECK(streamReadU16(in_data, writer_version, pos))

  uint8_t flags = 0xff;
  UHDR_ERR_CHECK(streamReadU8(in_data, flags, pos))
  const size_t channelCount = ((flags & kIsMultiChannelMask) != 0) ? 3u : 1u;
  out_metadata->useBaseColorSpace = (flags & kUseBaseColorSpaceMask) != 0;
  out_metadata->backwardDirection = (flags & 4) != 0;
  const bool useCommonDenominator = (flags & 8) != 0;

  if (useCommonDenominator) {
    uint32_t commonDenominator = 1u;
    UHDR_ERR_CHECK(streamReadU32(in_data, commonDenominator, pos))
    if (commonDenominator == 0) {
      return makeError(UHDR_CODEC_INVALID_PARAM,
                       "decoded gain map common denominator cannot be 0");
    }

    UHDR_ERR_CHECK(streamReadU32(in_data, out_metadata->baseHdrHeadroomN, pos))
    out_metadata->baseHdrHeadroomD = commonDenominator;
    UHDR_ERR_CHECK(streamReadU32(in_data, out_metadata->alternateHdrHeadroomN, pos))
    out_metadata->alternateHdrHeadroomD = commonDenominator;

    for (size_t c = 0; c < channelCount; ++c) {
      UHDR_ERR_CHECK(streamReadS32(in_data, out_metadata->gainMapMinN[c], pos))
      out_metadata->gainMapMinD[c] = commonDenominator;
      UHDR_ERR_CHECK(streamReadS32(in_data, out_metadata->gainMapMaxN[c], pos))
      out_metadata->gainMapMaxD[c] = commonDenominator;
      UHDR_ERR_CHECK(streamReadU32(in_data, out_metadata->gainMapGammaN[c], pos))
      out_metadata->gainMapGammaD[c] = commonDenominator;
      UHDR_ERR_CHECK(streamReadS32(in_data, out_metadata->baseOffsetN[c], pos))
      out_metadata->baseOffsetD[c] = commonDenominator;
      UHDR_ERR_CHECK(streamReadS32(in_data, out_metadata->alternateOffsetN[c], pos))
      out_metadata->alternateOffsetD[c] = commonDenominator;
    }
  } else {
    UHDR_ERR_CHECK(streamReadU32(in_data, out_metadata->baseHdrHeadroomN, pos))
    UHDR_ERR_CHECK(streamReadU32(in_data, out_metadata->baseHdrHeadroomD, pos))
    UHDR_ERR_CHECK(streamReadU32(in_data, out_metadata->alternateHdrHeadroomN, pos))
    UHDR_ERR_CHECK(streamReadU32(in_data, out_metadata->alternateHdrHeadroomD, pos))
    for (size_t c = 0; c < channelCount; ++c) {
      UHDR_ERR_CHECK(streamReadS32(in_data, out_metadata->gainMapMinN[c], pos))
      UHDR_ERR_CHECK(streamReadU32(in_data, out_metadata->gainMapMinD[c], pos))
      UHDR_ERR_CHECK(streamReadS32(in_data, out_metadata->gainMapMaxN[c], pos))
      UHDR_ERR_CHECK(streamReadU32(in_data, out_metadata->gainMapMaxD[c], pos))
      UHDR_ERR_CHECK(streamReadU32(in_data, out_metadata->gainMapGammaN[c], pos))
      UHDR_ERR_CHECK(streamReadU32(in_data, out_metadata->gainMapGammaD[c], pos))
      UHDR_ERR_CHECK(streamReadS32(in_data, out_metadata->baseOffsetN[c], pos))
      UHDR_ERR_CHECK(streamReadU32(in_data, out_metadata->baseOffsetD[c], pos))
      UHDR_ERR_CHECK(streamReadS32(in_data, out_metadata->alternateOffsetN[c], pos))
      UHDR_ERR_CHECK(streamReadU32(in_data, out_metadata->alternateOffsetD[c], pos))
    }
  }

  // Applies to both layouts: a zero denominator yields NaN / SIGFPE once the
  // rationals are converted to float, so reject it at parse time.
  if (out_metadata->baseHdrHeadroomD == 0 || out_metadata->alternateHdrHeadroomD == 0) {
    return makeError(UHDR_CODEC_INVALID_PARAM,
                     "decoded gain map headroom denominator cannot be 0");
  }
  for (size_t c = 0; c < channelCount; ++c) {
    if (out_metadata->gainMapMinD[c] == 0 || out_metadata->gainMapMaxD[c] == 0 ||
        out_metadata->gainMapGammaD[c] == 0 || out_metadata->baseOffsetD[c] == 0 ||
        out_metadata->alternateOffsetD[c] == 0) {
      std::stringstream ss;
      ss << "decoded gain map channel " << c << " denominator cannot be 0";
      return makeError(UHDR_CODEC_INVALID_PARAM, ss.str());
    }
  }

  // Fill the remaining values by copying those from the first channel.
  for (size_t c = channelCount; c < 3; ++c) {
    out_metadata->gainMapMinN[c] = out_metadata->gainMapMinN[0];
    out_metadata->gainMapMinD[c] = out_metadata->gainMapMinD[0];
    out_metadata->gainMapMaxN[c] = out_metadata->gainMapMaxN[0];
    out_metadata->gainMapMaxD[c] = out_metadata->gainMapMaxD[0];
    out_metadata->gainMapGammaN[c] = out_metadata->gainMapGammaN[0];
    out_metadata->gainMapGammaD[c] = out_metadata->gainMapGammaD[0];
    out_metadata->baseOffsetN[c] = out_metadata->baseOffsetN[0];
    out_metadata->baseOffsetD[c] = out_metadata->baseOffsetD[0];
    out_metadata->alternateOffsetN[c] = out_metadata->alternateOffsetN[0];
    out_metadata->alternateOffsetD[c] = out_metadata->alternateOffsetD[0];
  }

  return g_no_error;
}

#define UHDR_CHECK_NON_ZERO(x, message)                                                \
  if ((x) == 0) {                                                                      \
    return makeError(UHDR_CODEC_INVALID_PARAM,                                         \
                     std::string("received 0 (bad value) for field ") + (message));    \
  }

static uhdr_error_info_t invalidGainMapMetadata(std::string_view detail) {
  return makeError(UHDR_CODEC_INVALID_PARAM, detail);
}

uhdr_error_info_t uhdr_gainmap_metadata_frac::gainmapMetadataFractionToFloat(
    const uhdr_gainmap_metadata_frac *from, uhdr_gainmap_metadata_ext_t *to) {
  if (from == nullptr || to == nullptr) {
    return makeError(UHDR_CODEC_INVALID_PARAM,
                     "received nullptr for gain map metadata descriptor");
  }

  UHDR_CHECK_NON_ZERO(from->baseHdrHeadroomD, "baseHdrHeadroom denominator");
  UHDR_CHECK_NON_ZERO(from->alternateHdrHeadroomD, "alternateHdrHeadroom denominator");
  for (size_t i = 0; i < 3; ++i) {
    UHDR_CHECK_NON_ZERO(from->gainMapMaxD[i], "gainMapMax denominator");
    UHDR_CHECK_NON_ZERO(from->gainMapGammaD[i], "gainMapGamma denominator");
    UHDR_CHECK_NON_ZERO(from->gainMapMinD[i], "gainMapMin denominator");
    UHDR_CHECK_NON_ZERO(from->baseOffsetD[i], "baseOffset denominator");
    UHDR_CHECK_NON_ZERO(from->alternateOffsetD[i], "alternateOffset denominator");
    if (static_cast<int64_t>(from->gainMapMaxN[i]) * from->gainMapMinD[i] <
        static_cast<int64_t>(from->gainMapMinN[i]) * from->gainMapMaxD[i]) {
      return invalidGainMapMetadata(
          "decoded gain map maximum content boost is less than its minimum");
    }
  }

  // jpeg supports only 8 bits per component, applying gainmap in inverse direction is unexpected
  if (from->backwardDirection) {
    return makeError(UHDR_CODEC_UNSUPPORTED_FEATURE,
                     "hdr intent as base rendition is not supported");
  }

  to->version = kJpegrVersion;
  std::array<float, 3> max_content_boost{};
  std::array<float, 3> min_content_boost{};
  std::array<float, 3> gamma{};
  std::array<float, 3> offset_sdr{};
  std::array<float, 3> offset_hdr{};
  for (size_t i = 0; i < 3; i++) {
    max_content_boost[i] = exp2(static_cast<float>(from->gainMapMaxN[i]) / from->gainMapMaxD[i]);
    min_content_boost[i] = exp2(static_cast<float>(from->gainMapMinN[i]) / from->gainMapMinD[i]);

    gamma[i] = static_cast<float>(from->gainMapGammaN[i]) / from->gainMapGammaD[i];

    // BaseRenditionIsHDR is false
    offset_sdr[i] = static_cast<float>(from->baseOffsetN[i]) / from->baseOffsetD[i];
    offset_hdr[i] = static_cast<float>(from->alternateOffsetN[i]) / from->alternateOffsetD[i];
  }
  std::copy(max_content_boost.begin(), max_content_boost.end(), std::begin(to->max_content_boost));
  std::copy(min_content_boost.begin(), min_content_boost.end(), std::begin(to->min_content_boost));
  std::copy(gamma.begin(), gamma.end(), std::begin(to->gamma));
  std::copy(offset_sdr.begin(), offset_sdr.end(), std::begin(to->offset_sdr));
  std::copy(offset_hdr.begin(), offset_hdr.end(), std::begin(to->offset_hdr));

  to->hdr_capacity_max =
      exp2(static_cast<float>(from->alternateHdrHeadroomN) / from->alternateHdrHeadroomD);
  to->hdr_capacity_min = exp2(static_cast<float>(from->baseHdrHeadroomN) / from->baseHdrHeadroomD);
  to->use_base_cg = from->useBaseColorSpace;

  return uhdr_validate_gainmap_metadata_descriptor(to);
}

uhdr_error_info_t uhdr_gainmap_metadata_frac::gainmapMetadataFloatToFraction(
    const uhdr_gainmap_metadata_ext_t *from, uhdr_gainmap_metadata_frac *to) {
  if (from == nullptr || to == nullptr) {
    return makeError(UHDR_CODEC_INVALID_PARAM,
                     "received nullptr for gain map metadata descriptor");
  }

  to->backwardDirection = false;
  to->useBaseColorSpace = from->use_base_cg;

  auto convertUnsigned = [](float flt, uint32_t *numerator,
                            uint32_t *denominator) -> uhdr_error_info_t {
    if (!floatToUnsignedFraction(flt, numerator, denominator)) {
      std::stringstream ss;
      ss << "encountered error while representing float " << flt
         << " as a rational number (p/q form) ";
      return makeError(UHDR_CODEC_INVALID_PARAM, ss.str());
    }
    return g_no_error;
  };

  auto convertSigned = [](float flt, int32_t *numerator,
                          uint32_t *denominator) -> uhdr_error_info_t {
    if (!floatToSignedFraction(flt, numerator, denominator)) {
      std::stringstream ss;
      ss << "encountered error while representing float " << flt
         << " as a rational number (p/q form) ";
      return makeError(UHDR_CODEC_INVALID_PARAM, ss.str());
    }
    return g_no_error;
  };

  std::array<float, 3> max_content_boost{};
  std::array<float, 3> min_content_boost{};
  std::array<float, 3> gamma{};
  std::array<float, 3> offset_sdr{};
  std::array<float, 3> offset_hdr{};
  std::copy(std::begin(from->max_content_boost), std::end(from->max_content_boost),
            max_content_boost.begin());
  std::copy(std::begin(from->min_content_boost), std::end(from->min_content_boost),
            min_content_boost.begin());
  std::copy(std::begin(from->gamma), std::end(from->gamma), gamma.begin());
  std::copy(std::begin(from->offset_sdr), std::end(from->offset_sdr), offset_sdr.begin());
  std::copy(std::begin(from->offset_hdr), std::end(from->offset_hdr), offset_hdr.begin());

  bool isSingleChannel = from->are_all_channels_identical();
  for (size_t i = 0; i < (isSingleChannel ? 1u : 3u); i++) {
    UHDR_ERR_CHECK(
        convertSigned(log2(max_content_boost[i]), &to->gainMapMaxN[i], &to->gainMapMaxD[i]))
    UHDR_ERR_CHECK(
        convertSigned(log2(min_content_boost[i]), &to->gainMapMinN[i], &to->gainMapMinD[i]))
    UHDR_ERR_CHECK(convertUnsigned(gamma[i], &to->gainMapGammaN[i], &to->gainMapGammaD[i]))
    UHDR_ERR_CHECK(convertSigned(offset_sdr[i], &to->baseOffsetN[i], &to->baseOffsetD[i]))
    UHDR_ERR_CHECK(
        convertSigned(offset_hdr[i], &to->alternateOffsetN[i], &to->alternateOffsetD[i]))
  }

  if (isSingleChannel) {
    to->gainMapMaxN.fill(to->gainMapMaxN[0]);
    to->gainMapMaxD.fill(to->gainMapMaxD[0]);
    to->gainMapMinN.fill(to->gainMapMinN[0]);
    to->gainMapMinD.fill(to->gainMapMinD[0]);
    to->gainMapGammaN.fill(to->gainMapGammaN[0]);
    to->gainMapGammaD.fill(to->gainMapGammaD[0]);
    to->baseOffsetN.fill(to->baseOffsetN[0]);
    to->baseOffsetD.fill(to->baseOffsetD[0]);
    to->alternateOffsetN.fill(to->alternateOffsetN[0]);
    to->alternateOffsetD.fill(to->alternateOffsetD[0]);
  }

  UHDR_ERR_CHECK(
      convertUnsigned(log2(from->hdr_capacity_min), &to->baseHdrHeadroomN, &to->baseHdrHeadroomD))
  UHDR_ERR_CHECK(convertUnsigned(log2(from->hdr_capacity_max), &to->alternateHdrHeadroomN,
                                 &to->alternateHdrHeadroomD))

  return g_no_error;
}

}  // namespace ultrahdr
