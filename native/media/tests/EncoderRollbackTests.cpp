#include "telestudio/media/IVideoEncoder.h"
#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
  try {
    for (auto codec : {telestudio::VideoCodec::H264, telestudio::VideoCodec::H265}) {
      auto encoder = codec == telestudio::VideoCodec::H264
          ? telestudio::CreateX264Encoder(TELESTUDIO_CODEC_BIN_DIR)
          : telestudio::CreateX265Encoder(TELESTUDIO_CODEC_BIN_DIR);
      encoder->Open({codec, 128, 64, 30, 128, false, 60});
      std::vector<uint8_t> y(8192, 80), u(2048, 128), v(2048, 128);
      telestudio::I420Frame frame{y.data(),u.data(),v.data(),128,64,64,128,64,1};
      if (encoder->Encode(frame, false).annexB.empty()) throw std::runtime_error("initial encode");
      bool rejected = false;
      // Valid wrapper inputs but rejected by the codec, beyond codec dimension limits.
      try { encoder->Reconfigure({codec, 32768, 2, 30, 128, false, 60}); }
      catch (const std::exception&) { rejected = true; }
      if (!rejected) throw std::runtime_error("fixture must reject at codec open");
      frame.timestamp100ns = 2;
      if (encoder->Encode(frame, false).annexB.empty()) throw std::runtime_error("old encoder lost on rejected resize");
      encoder->Reconfigure({codec, 64, 64, 30, 128, false, 60});
      frame.width = 64; frame.timestamp100ns = 3;
      if (!encoder->Encode(frame, false).keyFrame) throw std::runtime_error("resize must force IDR");
    }
    std::cout << "encoder rollback: PASS\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
