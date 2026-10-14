#include "mkvCopyStreamReader.h"

#include <cstring>
#include <sstream>

#include "vodCoreException.h"
#include "vod_common.h"

namespace
{
uint64_t readBe(const uint8_t* p, const int bytes)
{
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = v << 8 | p[i];
    return v;
}
}  // namespace

MatroskaCopyStreamReader::MatroskaCopyStreamReader(const std::string& codecId, std::vector<uint8_t> codecPrivate,
                                                   std::vector<std::vector<uint8_t>> addMappings,
                                                   const uint64_t maxBlockAddId)
    : m_codecInfo(CODEC_S_MKVCOPY, "Matroska " + codecId, "S_MKVCOPY"),
      m_sourceCodecId(codecId),
      m_codecPrivate(std::move(codecPrivate)),
      m_addMappings(std::move(addMappings)),
      m_maxBlockAddId(maxBlockAddId)
{
}

void MatroskaCopyStreamReader::setBuffer(uint8_t* data, const uint32_t dataLen, bool /*lastBlock*/)
{
    // What is left of the last delivery first, then the new bytes, which start getTmpBufferSize() bytes into data
    if (m_inputPos > 0)
    {
        m_input.erase(m_input.begin(), m_input.begin() + static_cast<std::ptrdiff_t>(m_inputPos));
        m_inputPos = 0;
    }
    if (data && dataLen > 0)
        m_input.insert(m_input.end(), data + MAX_AV_PACKET_SIZE, data + MAX_AV_PACKET_SIZE + dataLen);
}

int MatroskaCopyStreamReader::readPacket(AVPacket& avPacket)
{
    avPacket.codec = this;
    avPacket.codecID = CODEC_S_MKVCOPY;
    avPacket.stream_index = m_streamIndex;
    avPacket.data = nullptr;
    avPacket.size = 0;
    avPacket.flags = 0;
    avPacket.duration = 0;

    static constexpr size_t HEAD = 4 + 8 + 8 + 1 + 4;
    const size_t left = m_input.size() - m_inputPos;
    if (left < HEAD)
        return NEED_MORE_DATA;
    const uint8_t* p = m_input.data() + m_inputPos;
    if (memcmp(p, "MKCP", 4) != 0)
        THROW(ERR_COMMON,
              "A Matroska track copied as it is lost its framing at byte " << m_processedSize << " of its data.")
    const auto pts = static_cast<int64_t>(readBe(p + 4, 8));
    const auto duration = static_cast<int64_t>(readBe(p + 12, 8));
    const bool keyFrame = p[20] != 0;
    const auto addSize = static_cast<size_t>(readBe(p + 21, 4));
    if (left < HEAD + addSize + 4)
        return NEED_MORE_DATA;
    const auto dataSize = static_cast<size_t>(readBe(p + HEAD + addSize, 4));
    const size_t record = HEAD + addSize + 4 + dataSize;
    if (left < record)
        return NEED_MORE_DATA;

    m_packet.resize(4 + addSize + dataSize);
    for (int i = 0; i < 4; ++i) m_packet[i] = static_cast<uint8_t>(addSize >> (8 * (3 - i)));
    if (addSize)
        memcpy(m_packet.data() + 4, p + HEAD, addSize);
    if (dataSize)
        memcpy(m_packet.data() + 4 + addSize, p + HEAD + addSize + 4, dataSize);

    avPacket.pts = avPacket.dts = pts;
    avPacket.duration = duration > 0 ? duration : 0;
    avPacket.flags = keyFrame ? AVPacket::IS_IFRAME : 0;
    avPacket.data = m_packet.data();
    avPacket.size = static_cast<int>(m_packet.size());
    m_inputPos += record;
    m_processedSize += static_cast<int64_t>(record);
    ++m_frames;
    return 0;
}

int MatroskaCopyStreamReader::flushPacket(AVPacket& avPacket)
{
    const int rez = readPacket(avPacket);
    if (rez == NEED_MORE_DATA)
    {
        avPacket.data = nullptr;
        avPacket.size = 0;
        return 0;
    }
    return rez;
}
