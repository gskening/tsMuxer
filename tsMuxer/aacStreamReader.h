#ifndef AAC_STREAM_READER_H_
#define AAC_STREAM_READER_H_

#include "aac.h"
#include "simplePacketizerReader.h"

class AACStreamReader final : public SimplePacketizerReader, public AACCodec
{
   public:
    AACStreamReader() = default;
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override;
    int getFreq() override { return m_sample_rate; }
    uint8_t getChannels() override { return m_channels; }
    void applyDiscoveryData(const StreamDiscoveryData& data) override;
    void fillDiscoveryData(StreamDiscoveryData& data) override;
    CheckStreamRez checkStream(uint8_t* buffer, int len, ContainerType containerType, int containerDataType,
                               int containerStreamIndex) override;

   protected:
    int getHeaderLen() override;
    int decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes) override;
    uint8_t* findFrame(uint8_t* buff, uint8_t* end) override { return findAacFrame(buff, end, m_tagCeilingEnd); }
    double getFrameDuration() override { return static_cast<double>(m_samples) * INTERNAL_PTS_FREQ / m_sample_rate; }
    const CodecInfo& getCodecInfo() override { return aacCodecInfo; }
    const std::string getStreamInfo() override;

   private:
    // What the listing saw: the bits and samples of every frame in the data it was given.
    int64_t m_seenBits = 0;
    int64_t m_seenSamples = 0;
};

#endif
