#include "vpxStreamReader.h"

#include <algorithm>
#include <sstream>

#include "avCodecs.h"
#include "bitStream.h"
#include "nalUnits.h"

VpxStreamReader::VpxStreamReader(const bool vp9)
    : m_vp9(vp9), m_width(0), m_height(0), m_profile(0), m_bitDepth(8), m_lastIFrame(false), m_firstFrame(true)
{
}

const CodecInfo& VpxStreamReader::getCodecInfo() { return m_vp9 ? vp9CodecInfo : vp8CodecInfo; }

std::vector<uint8_t> VpxStreamReader::unframe(const uint8_t* data, const int size)
{
    const uint8_t* p = data;
    const uint8_t* end = data + size;
    while (p < end && *p == 0) ++p;  // the start code, three or four bytes
    if (p < end && *p == 1)
        ++p;
    while (end > p && end[-1] == 0) --end;  // nothing follows the stop byte; this is only care
    if (end > p && end[-1] == 0x80)
        --end;
    std::vector<uint8_t> frame(end - p);
    if (!frame.empty())
        frame.resize(NALUnit::decodeNAL(p, end, frame.data(), frame.size()));
    return frame;
}

bool VpxStreamReader::parseFrameHeader(const uint8_t* frame, const int size)
{
    if (!m_vp9)
    {
        // RFC 6386, 9.1: a three byte frame tag, its lowest bit 0 for a keyframe, which goes on with the start code
        // 9D 01 2A and the size, 14 bits each with two bits of scaling above them.
        if (size < 10 || (frame[0] & 1) != 0 || frame[3] != 0x9D || frame[4] != 0x01 || frame[5] != 0x2A)
            return false;
        m_profile = frame[0] >> 1 & 7;
        m_width = (frame[6] | frame[7] << 8) & 0x3FFF;
        m_height = (frame[8] | frame[9] << 8) & 0x3FFF;
        return true;
    }

    // VP9, the uncompressed header: frame marker, profile, show_existing_frame, frame_type (0 for a keyframe),
    // show_frame and error_resilient_mode, then for a keyframe the sync code 49 83 42, the colour configuration and
    // the frame size. The first frame of a superframe stands at the start of the block.
    try
    {
        // A copy with room after it: the reader fetches four bytes at a time.
        std::vector<uint8_t> copy(frame, frame + size);
        copy.resize(size + 8, 0);
        BitStreamReader bits{};
        bits.setBuffer(copy.data(), copy.data() + size);
        if (bits.getBits(2) != 2)
            return false;
        const int profileLow = bits.getBit();
        const int profile = (bits.getBit() << 1) | profileLow;
        if (profile == 3)
            bits.skipBit();
        if (bits.getBit())  // show_existing_frame
            return false;
        if (bits.getBit())  // frame_type 1: not a keyframe
            return false;
        bits.skipBits(2);  // show_frame, error_resilient_mode
        if (bits.getBits(24) != 0x498342)
            return false;
        int bitDepth = 8;
        if (profile >= 2)
            bitDepth = bits.getBit() ? 12 : 10;
        const unsigned colourSpace = bits.getBits(3);
        if (colourSpace != 7)  // not sRGB
        {
            bits.skipBit();  // colour range
            if (profile == 1 || profile == 3)
                bits.skipBits(3);  // subsampling x and y, reserved
        }
        else if (profile == 1 || profile == 3)
            bits.skipBit();
        m_width = bits.getBits(16) + 1;
        m_height = bits.getBits(16) + 1;
        m_profile = profile;
        m_bitDepth = bitDepth;
        return true;
    }
    catch (BitStreamException&)
    {
        return false;
    }
}

void VpxStreamReader::incTimings()
{
    if (m_totalFrameNum++ > 0)
        m_curDts += m_pcrIncPerFrame;
    m_curPts = m_curDts;  // neither codec reorders: each frame is shown when it is decoded
}

int VpxStreamReader::intDecodeNAL(uint8_t* buff)
{
    // One unit is one frame. Its header is read from its first bytes, unescaped.
    uint8_t* frameEnd = NALUnit::findNALWithStartCode(buff, m_bufEnd, m_longCodesAllowed);
    if (!m_eof && frameEnd == m_bufEnd)
        return NOT_ENOUGH_BUFFER;
    uint8_t header[96];
    const int headerLen = NALUnit::decodeNAL(buff, (std::min)(frameEnd, buff + 64), header, sizeof header);
    if (m_firstFrame)
    {
        updateFPS(nullptr, buff, frameEnd, 0);  // the container's rate, fps= or 25 with a message
        m_firstFrame = false;
    }
    const unsigned width = m_width;
    const unsigned height = m_height;
    m_lastIFrame = parseFrameHeader(header, headerLen);
    if (width != 0 && (m_width != width || m_height != height))
    {
        // A later keyframe of another size. The track is described by its first size.
        m_width = width;
        m_height = height;
    }
    m_spsPpsFound = false;
    incTimings();
    return 0;
}

CheckStreamRez VpxStreamReader::checkStream(uint8_t* buffer, const int len)
{
    CheckStreamRez rez;
    uint8_t* end = buffer + len;
    uint8_t* frame = NALUnit::findNextNAL(buffer, end);
    if (frame < end)
    {
        uint8_t* frameEnd = NALUnit::findNALWithStartCode(frame, end, true);
        uint8_t header[96];
        const int headerLen = NALUnit::decodeNAL(frame, (std::min)(frameEnd, frame + 64), header, sizeof header);
        if (parseFrameHeader(header, headerLen) && m_width > 0 && m_height > 0)
        {
            rez.codecInfo = getCodecInfo();
            std::ostringstream str;
            str << "Profile: " << m_profile << "  Resolution: " << m_width << ":" << m_height << "p";
            if (m_vp9)
                str << "  Bit depth: " << m_bitDepth << "bit";
            str << "  Frame rate: not found";
            rez.streamDescr = str.str();
        }
    }
    m_totalFrameNum = 0;
    m_curDts = m_curPts = 0;
    return rez;
}
