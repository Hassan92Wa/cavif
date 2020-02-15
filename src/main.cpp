#include <iostream>

#include "avif/img/Image.hpp"
#include "img/PNGReader.hpp"
#include "AVIFBuilder.hpp"
#include <aom/aom_encoder.h>
#include <aom/aom_codec.h>
#include <aom/aomcx.h>
#include <avif/av1/Parser.hpp>
#include <avif/util/FileLogger.hpp>
#include <avif/util/File.hpp>
#include <avif/FileBox.hpp>
#include <avif/Writer.hpp>
#include <thread>

#include "Config.hpp"
#include "img/Convertion.hpp"

namespace {

// FIXME(ledyba-z): remove this function when the C++20 comes.
bool endsWidh(std::string const& target, std::string const& suffix) {
  if(target.size() < suffix.size()) {
    return false;
  }
  return target.substr(target.size()-suffix.size()) == suffix;
}

size_t encode(avif::util::Logger& log, aom_codec_ctx_t& codec, aom_image* img, std::vector<std::vector<uint8_t>>& packets) {
  aom_codec_cx_pkt_t const* pkt;
  aom_codec_iter_t iter = nullptr;
  aom_codec_err_t const res = aom_codec_encode(&codec, img, 0, 1, img ? AOM_EFLAG_FORCE_KF : 0);
  if (res != AOM_CODEC_OK) {
    if(img) {
      log.fatal("failed to encode a frame: %s", aom_codec_error_detail(&codec));
    } else {
      log.fatal("failed to flush encoder: %s", aom_codec_error_detail(&codec));
    }
    return 0;
  }
  size_t numPackets = 0;
  while ((pkt = aom_codec_get_cx_data(&codec, &iter)) != nullptr) {
    if (pkt->kind == AOM_CODEC_CX_FRAME_PKT) {
      auto& frame = pkt->data.frame;
      auto const beg = reinterpret_cast<uint8_t*>(frame.buf);
      packets.emplace_back(std::vector<uint8_t>(beg, beg + frame.sz));
      ++numPackets;
    }
  }
  return numPackets;
}

}

static int _main(int argc, char** argv);
void printSequenceHeader(avif::util::Logger& log, avif::av1::SequenceHeader& seq);

int main(int argc, char** argv) {
  try {
    return _main(argc, argv);
  } catch (std::exception& err) {
    fprintf(stderr, "%s\n", err.what());
    fflush(stderr);
    return -1;
  }
}

int _main(int argc, char** argv) {
  avif::util::FileLogger log(stdout, stderr, avif::util::Logger::Level::DEBUG);
  log.info("cavif");
  log.info("libaom ver: %s", aom_codec_version_str());

  aom_codec_iface_t* av1codec = aom_codec_av1_cx();
  if(!av1codec) {
    log.fatal("failed to get AV1 encoder.");
  }

  Config config;
  aom_codec_flags_t flags = 0;
  aom_codec_enc_config_default(av1codec, &config.codec, 0);
  config.codec.g_threads = std::thread::hardware_concurrency();
  {
    int const parsrResult = config.parse(argc, argv);
    if(parsrResult != 0) {
      return parsrResult;
    }
  }

  // decoding input image
  if(!endsWidh(config.input, ".png")) {
    log.fatal("please give png file for input");
  }
  std::variant<avif::img::Image<8>, avif::img::Image<16>> loadedImage = PNGReader(config.input).read();

  aom_image_t img;
  aom_img_fmt_t pixFmt = config.pixFmt;
  if(config.codec.g_bit_depth > 8) {
    pixFmt = static_cast<aom_img_fmt_t>(pixFmt | static_cast<unsigned int>(AOM_IMG_FMT_HIGHBITDEPTH));
    flags = flags | AOM_CODEC_USE_HIGHBITDEPTH;
  }
  if(std::holds_alternative<avif::img::Image<8>>(loadedImage)) {
    auto src = std::get<avif::img::Image<8>>(loadedImage);
    aom_img_alloc(&img, pixFmt, src.width(), src.height(), 1);
    img.range = config.fullColorRange ? AOM_CR_FULL_RANGE : AOM_CR_STUDIO_RANGE;
    convert(src, img, config.codec.g_bit_depth);
  } else {
    auto src = std::get<avif::img::Image<16>>(loadedImage);
    aom_img_alloc(&img, pixFmt, src.width(), src.height(), 1);
    img.range = config.fullColorRange ? AOM_CR_FULL_RANGE : AOM_CR_STUDIO_RANGE;
    convert(src, img, config.codec.g_bit_depth);
  }

  uint32_t const width = aom_img_plane_width(&img, AOM_PLANE_Y);
  uint32_t const height = aom_img_plane_height(&img, AOM_PLANE_Y);

  // initialize encoder
  config.codec.g_w = width;
  config.codec.g_h = height;
  // Generate just one frame.
  config.codec.g_limit = 1;
  config.codec.g_pass = AOM_RC_ONE_PASS;
  // FIXME(ledyba-z): Encoder produces wrong images when g_input_bit_depth != g_bit_depth. Bug?
  config.codec.g_input_bit_depth = config.codec.g_bit_depth;
  // FIXME(ledyba-z): If kf_max_dist = 1, it crashes. Bug?
  // > A value of 0 implies all frames will be keyframes.
  // However, when it is set to 0, assertion always fails:
  // cavif/external/libaom/av1/encoder/gop_structure.c:92:
  // construct_multi_layer_gf_structure: Assertion `gf_interval >= 1' failed.
  config.codec.kf_max_dist = 1;
  // One frame takes 1 second.
  config.codec.g_timebase.den = 1;
  config.codec.g_timebase.num = 1;
  //
  config.codec.rc_target_bitrate = 0;

  aom_codec_ctx_t codec{};

  if(AOM_CODEC_OK != aom_codec_enc_init(&codec, av1codec, &config.codec, flags)) {
    log.fatal("Failed to initialize encoder: %s", aom_codec_error_detail(&codec));
  }

  config.modify(&codec);


  std::vector<std::vector<uint8_t>> packets;
  {
    log.info("Encoding: %s -> %s", config.input, config.output);
    auto start = std::chrono::steady_clock::now();
    encode(log, codec, &img, packets);
    while(encode(log, codec, nullptr, packets) > 0); //flushing
    auto finish = std::chrono::steady_clock::now();
    log.info(" Encoded: %s -> %s in %.2f [sec]", config.input, config.output, std::chrono::duration_cast<std::chrono::milliseconds>(finish-start).count() / 1000.0f);
  }
  aom_img_free(&img);

  if (aom_codec_destroy(&codec) != AOM_CODEC_OK) {
    log.error("Failed to destroy codec: %s", aom_codec_error_detail(&codec));
    return -1;
  }

  if(packets.empty()) {
    log.error("no packats to out.");
    return -1;
  }
  {
    AVIFBuilder builder(config, width, height);
    std::shared_ptr<avif::av1::Parser::Result> result = avif::av1::Parser(log, packets[0]).parse();
    if (!result->ok()) {
      log.error(result->error());
      return -1;
    }
    std::optional<avif::av1::SequenceHeader> seq{};
    std::vector<uint8_t> configOBUs;
    std::vector<uint8_t> mdat;
    for(avif::av1::Parser::Result::Packet const& packet : result->packets()) {
      switch (packet.type()) {
        case avif::av1::Header::Type::TemporalDelimiter:
        case avif::av1::Header::Type::Padding:
        case avif::av1::Header::Type::Reserved:
          break;
        case avif::av1::Header::Type::SequenceHeader:
          seq = std::get<avif::av1::SequenceHeader>(packet.content());
          configOBUs.insert(std::end(configOBUs), std::next(std::begin(result->buffer()), packet.beg()), std::next(result->buffer().begin(), packet.end()));
          mdat.insert(std::end(mdat), std::next(std::begin(result->buffer()), packet.beg()), std::next(std::begin(result->buffer()), packet.end()));
          break;
        case avif::av1::Header::Type::Frame:
        default: {
          mdat.insert(std::end(mdat), std::next(std::begin(result->buffer()), packet.beg()), std::next(std::begin(result->buffer()), packet.end()));
          break;
        }
      }
    }
    if (!seq.has_value()) {
      throw std::logic_error("No sequence header OBU.");
    }
    builder.setPrimaryFrame(AVIFBuilder::Frame(seq.value(), std::move(configOBUs), mdat));
    avif::FileBox fileBox = builder.build();
    {
      avif::util::StreamWriter pass1;
      avif::Writer(log, pass1).write(fileBox);
    }
    for (size_t i = 0; i < fileBox.metaBox.itemLocationBox.items.size(); ++i) {
      size_t const offset = fileBox.mediaDataBoxes.at(i).offset;
      fileBox.metaBox.itemLocationBox.items.at(i).baseOffset = offset;
    }
    avif::util::StreamWriter out;
    avif::Writer(log, out).write(fileBox);
    std::vector<uint8_t> data = out.buffer();
    std::copy(std::begin(mdat), std::end(mdat), std::next(std::begin(data), fileBox.mediaDataBoxes.at(0).offset));
    std::optional<std::string> writeResult = avif::util::writeFile(config.output, data);
    if (writeResult.has_value()) {
      log.error(writeResult.value());
      return -1;
    }
    if (config.showResult) {
      printSequenceHeader(log, seq.value());
    }
  }
  return 0;
}

void printSequenceHeader(avif::util::Logger& log, avif::av1::SequenceHeader& seq) {
  log.info("<Encoding Result>");
  log.info(" - OBU Sequence Header:");
  log.info("   - AV1 Profile: %d", seq.seqProfile);
  log.info("   - Still picture: %s", seq.stillPicture ? "Yes" : "No");
  log.info("   - Reduced still picture header: %s", seq.reducedStillPictureHeader ? "Yes" : "No");
  log.info("   - Sequence Level Index at OperatingPoint[0]: %d", seq.operatingPoints.at(0).seqLevelIdx);
  log.info("   - Max frame width: %d", seq.maxFrameWidth);
  log.info("   - Max frame height: %d", seq.maxFrameHeight);
  log.info("   - Use 128x128 superblock: %s", seq.use128x128Superblock ? "Yes" : "No");
  log.info("   - FilterIntra enabled: %s", seq.enableFilterIntra ? "Yes" : "No");
  log.info("   - IntraEdgeFilter enabled: %s", seq.enableIntraEdgeFilter ? "Yes" : "No");
/*
  log.info("   - InterIntraCompound enabled: %s", seq.enableInterintraCompound ? "Yes" : "No");
  log.info("   - Masked Compound enabled: %s", seq.enableMaskedCompound ? "Yes" : "No");
  log.info("   - WarpedMotion enabled: %s", seq.enableWarpedMotion ? "Yes" : "No");
  log.info("   - DualFilter enabled: %s", seq.enableDualFilter ? "Yes" : "No");
  log.info("   - OrderHint enabled: %s", seq.enableOrderHint ? "Yes" : "No");
  log.info("   - JNTComp enabled: %s", seq.enableJNTComp ? "Yes" : "No");
  log.info("   - RefFrameMVS enabled: %s", seq.enableRefFrameMVS ? "Yes" : "No");
*/
  log.info("   - Superres enabled: %s", seq.enableSuperres ? "Yes" : "No");
  log.info("   - CDEF enabled: %s", seq.enableCDEF ? "Yes" : "No");
  log.info("   - Loop Restoration enabled: %s", seq.enableRestoration ? "Yes" : "No");
  log.info("   - Film Grain Params Present: %s", seq.filmGrainParamsPresent ? "Yes" : "No");
  log.info("   - Color Info:");
  log.info("     - High bit-depth: %s", seq.colorConfig.highBitdepth ? "Yes" : "No");
  log.info("     - Twelve bit: %s", seq.colorConfig.twelveBit ? "Yes" : "No");
  log.info("     - Monochrome: %s", seq.colorConfig.monochrome ? "Yes" : "No");
  log.info("     - Color primaries: %s", seq.colorConfig.colorPrimaries.has_value() ? std::to_string(static_cast<uint8_t>(seq.colorConfig.colorPrimaries.value())) : "<Unknownn>");
  log.info("     - Transfer characteristics: %s", seq.colorConfig.transferCharacteristics.has_value() ? std::to_string(static_cast<uint8_t>(seq.colorConfig.transferCharacteristics.value())) : "<Unknownn>");
  log.info("     - Matrix coefficients: %s", seq.colorConfig.matrixCoefficients.has_value() ? std::to_string(static_cast<uint8_t>(seq.colorConfig.matrixCoefficients.value())) : "<Unknownn>");
  log.info("     - Color range: %s", seq.colorConfig.colorRange ? "Full Ranged" : "Limited");
  log.info("     - Sub sampling X: %d", seq.colorConfig.subsamplingX);
  log.info("     - Sub sampling Y: %d", seq.colorConfig.subsamplingX);
  log.info("     - Chroma sample position: %s", seq.colorConfig.chromaSamplePosition.has_value() ? std::to_string(static_cast<uint8_t>(seq.colorConfig.chromaSamplePosition.value())) : "<Unknownn>");
  log.info("     - Separate UV Delta Q: %s", seq.colorConfig.separateUVDeltaQ ? "Yes" : "No");
}
