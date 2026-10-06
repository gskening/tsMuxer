#include "srtStreamReader.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <set>
#include <sstream>
#include <string>

#include "convertUTF.h"
#include "fs/systemlog.h"
#include "matroskaParser.h"
#include "vodCoreException.h"
#include "vod_common.h"

using namespace std;

using namespace text_subtitles;

SRTStreamReader::SRTStreamReader() : m_lastBlock(false), m_short_R(0), m_short_N(0), m_long_R(0), m_long_N(0)
{
    // in future version here must be case for destination subtitle format (DVB sub, DVD sub e.t.c)
    auto* pgsRender = new PGSStreamReader();
    pgsRender->setRenderedFromText(true);
    m_dstSubCodec = pgsRender;
    m_srtRender = new TextToPGSConverter(true);
    m_processedSize = 0;
    m_state = ParseState::PARSE_FIRST_LINE;
    m_inTime = m_outTime = 0.0;
    m_srcFormat = UtfConverter::SourceFormat::sfUnknown;
    m_charSize = 1;
    m_splitterOfs = 0;
    m_linesRead = 0;
}

SRTStreamReader::~SRTStreamReader()
{
    delete m_dstSubCodec;
    delete m_srtRender;
}

void SRTStreamReader::setAnimation(const TextAnimation& animation) { m_animation = animation; }

void SRTStreamReader::setBuffer(uint8_t* data, const uint32_t dataLen, const bool lastBlock)
{
    m_lastBlock = lastBlock;
    uint8_t* dataBegin = data + MAX_AV_PACKET_SIZE - m_tmpBuffer.size();
    if (!m_tmpBuffer.empty())
        memmove(dataBegin, m_tmpBuffer.data(), m_tmpBuffer.size());
    const int parsedLen = parseText(dataBegin, dataLen + m_tmpBuffer.size());
    const size_t rest = dataLen + m_tmpBuffer.size() - parsedLen;
    if (rest > MAX_AV_PACKET_SIZE)
        THROW(ERR_COMMON, "Invalid SRT file or too large text message (>" << MAX_AV_PACKET_SIZE << " bytes)")
    // The unfinished line starts where parsing stopped. Its position used to be worked out AFTER the
    // buffer had been resized to the new length, which pointed at the end of the data instead, so
    // the bytes kept were whatever followed the data: a line crossing the end of a read block was
    // corrupted, and so was the last line of a file that does not end with a line end.
    const uint8_t* restStart = dataBegin + parsedLen;
    m_tmpBuffer.resize(rest);
    if (rest > 0)
        memmove(m_tmpBuffer.data(), restStart, rest);

    // A line is only taken once its line end arrives, so the last line of a file that does not end
    // with one stayed in the buffer for good, and with it the last subtitle. At the end of the file
    // that line is finished here, with a line end in the file's own encoding.
    if (m_lastBlock && !m_tmpBuffer.empty())
    {
        std::vector<uint8_t> lastLine(m_tmpBuffer.begin(), m_tmpBuffer.end());
        if (m_charSize == 2)
        {
            const auto* nl = reinterpret_cast<const uint8_t*>(&m_short_N);
            lastLine.insert(lastLine.end(), nl, nl + 2);
        }
        else if (m_charSize == 4)
        {
            const auto* nl = reinterpret_cast<const uint8_t*>(&m_long_N);
            lastLine.insert(lastLine.end(), nl, nl + 4);
        }
        else
            lastLine.push_back('\n');
        parseText(lastLine.data(), lastLine.size());
        m_tmpBuffer.clear();
    }
}

bool SRTStreamReader::detectSrcFormat(const uint8_t* dataStart, const size_t len, int& prefixLen)
{
    prefixLen = 0;
    if (len < 4)
        return false;
    // detect UTF-8/UTF-16/UTF-32 format
    const bool hasUtf8Bom = dataStart[0] == 0xEF && dataStart[1] == 0xBB && dataStart[2] == 0xBF;
    if (hasUtf8Bom || convertUTF::isLegalUTF8String(dataStart, len))
    {
        m_charSize = 1;
        m_srcFormat = UtfConverter::SourceFormat::sfUTF8;
        // Only skip the byte order mark when there IS one. This branch is also how a plain UTF-8
        // file with no mark is recognised, and those three bytes were being thrown away from the
        // front of it. Every other branch here already sets prefixLen only on a mark it matched.
        if (hasUtf8Bom)
            prefixLen = 3;
    }
    else if (dataStart[0] == 0 && dataStart[1] == 0 && dataStart[2] == 0xFE && dataStart[3] == 0xFF)
    {
        m_srcFormat = UtfConverter::SourceFormat::sfUTF32be;
        m_charSize = 4;
        prefixLen = 4;
        m_splitterOfs = 3;
        m_long_N = my_htonl(0x0000000a);
        m_long_R = my_htonl(0x0000000d);
    }
    else if (dataStart[0] == 0xFF && dataStart[1] == 0xFE && dataStart[2] == 0 && dataStart[3] == 0)
    {
        m_srcFormat = UtfConverter::SourceFormat::sfUTF32le;
        m_charSize = 4;
        prefixLen = 4;
        m_long_N = my_htonl(0x0a000000);
        m_long_R = my_htonl(0x0d000000);
    }
    else if (dataStart[0] == 0xFE && dataStart[1] == 0xFF)
    {
        m_srcFormat = UtfConverter::SourceFormat::sfUTF16be;
        m_charSize = 2;
        prefixLen = 2;
        m_splitterOfs = 1;
        m_short_N = my_htons(0x000a);
        m_short_R = my_htons(0x000d);
    }
    else if (dataStart[0] == 0xFF && dataStart[1] == 0xFE)
    {
        m_srcFormat = UtfConverter::SourceFormat::sfUTF16le;
        m_charSize = 2;
        prefixLen = 2;
        m_short_N = my_htons(0x0a00);
        m_short_R = my_htons(0x0d00);
    }
    else
    {
#ifdef _WIN32
        LTRACE(LT_INFO, 2, "Failed to auto-detect SRT encoding : falling back to the active code page");
        m_srcFormat = UtfConverter::SourceFormat::sfANSI;  // default value for win32
#else
        LTRACE(LT_INFO, 2, "Failed to auto-detect SRT encoding : falling back to UTF-8");
        m_srcFormat = UtfConverter::SourceFormat::sfUTF8;
#endif
    }
    return true;
}

int SRTStreamReader::parseText(uint8_t* dataStart, const size_t len)
{
    int prefixLen = 0;
    if (m_srcFormat == UtfConverter::SourceFormat::sfUnknown)
    {
        if (!detectSrcFormat(dataStart, len, prefixLen))
            return false;
    }
    uint8_t* cur = dataStart + prefixLen;
    const size_t roundLen = len & (~(m_charSize - 1));
    const uint8_t* end = cur + roundLen;
    const uint8_t* lastProcessedLine = cur;
    vector<string> rez;
    for (; cur < end; cur += m_charSize)
    {
        // if (cur[m_splitterOfs] == '\n')
        if ((m_charSize == 1 && *cur == '\n') || (m_charSize == 2 && *reinterpret_cast<uint16_t*>(cur) == m_short_N) ||
            (m_charSize == 4 && *reinterpret_cast<uint32_t*>(cur) == m_long_N))
        {
            int32_t x = 0;
            if (cur >= m_charSize + lastProcessedLine)
                if ((m_charSize == 1 && cur[-1] == '\r') ||
                    (m_charSize == 2 && reinterpret_cast<uint16_t*>(cur)[-1] == m_short_R) ||
                    (m_charSize == 4 && reinterpret_cast<uint32_t*>(cur)[-1] == m_long_R))
                    x = m_charSize;

            m_sourceText.emplace_back(
                UtfConverter::toUtf8(lastProcessedLine, cur - lastProcessedLine - x, m_srcFormat));
            std::string& tmp = m_sourceText.back();
            if (strOnlySpace(tmp))
                tmp.clear();

            m_linesRead++;
            m_origSize.push(static_cast<int32_t>(cur + m_charSize - lastProcessedLine + prefixLen));
            prefixLen = 0;
            lastProcessedLine = cur + m_charSize;
        }
    }
    return static_cast<int>(lastProcessedLine - dataStart);
}

bool SRTStreamReader::strOnlySpace(const std::string& str)
{
    return std::all_of(str.begin(), str.end(), [](const char c) { return c == ' '; });
}

int SRTStreamReader::flushPacket(AVPacket& avPacket)
{
    if (!m_keepText)
        return m_dstSubCodec->flushPacket(avPacket);
    if (m_lengthlessCues > 1)
        LTRACE(LT_WARN, 2,
               "Warning: track " << m_streamIndex << ": " << m_lengthlessCues
                                 << " subtitles in all have no length and were left out.");
    if (m_invalidUtf8Cues > 1)
        LTRACE(LT_WARN, 2,
               "Warning: track " << m_streamIndex << ": " << m_invalidUtf8Cues
                                 << " subtitles in all held bytes that are not valid UTF-8.");
    m_lengthlessCues = m_invalidUtf8Cues = 0;
    avPacket.codec = this;
    avPacket.codecID = CODEC_S_SRT;
    avPacket.stream_index = m_streamIndex;
    avPacket.data = nullptr;
    avPacket.size = 0;
    avPacket.duration = 0;
    return 0;
}

// Valid UTF-8, which a Matroska text track must hold: a byte that does not begin or continue a valid
// sequence (RFC 3629) becomes U+FFFD. The text has already been converted from the file's encoding;
// this catches a file whose encoding was judged from its first part and changes later, and a file
// that is not UTF-8 on a system with no code page to fall back to. Returns whether a byte was replaced.
static bool makeValidUtf8(std::string& text)
{
    std::string out;
    bool replaced = false;
    size_t i = 0;
    while (i < text.size())
    {
        const auto b = static_cast<uint8_t>(text[i]);
        size_t len = 0;
        uint8_t lo = 0x80;  // the allowed range of the second byte
        uint8_t hi = 0xBF;
        if (b < 0x80)
            len = 1;
        else if (b >= 0xC2 && b <= 0xDF)
            len = 2;
        else if (b >= 0xE0 && b <= 0xEF)
        {
            len = 3;
            if (b == 0xE0)
                lo = 0xA0;  // no overlong form
            else if (b == 0xED)
                hi = 0x9F;  // no surrogate
        }
        else if (b >= 0xF0 && b <= 0xF4)
        {
            len = 4;
            if (b == 0xF0)
                lo = 0x90;
            else if (b == 0xF4)
                hi = 0x8F;  // nothing above U+10FFFF
        }
        bool ok = len > 0 && i + len <= text.size();
        for (size_t k = 1; ok && k < len; ++k)
        {
            const auto c = static_cast<uint8_t>(text[i + k]);
            ok = k == 1 ? c >= lo && c <= hi : c >= 0x80 && c <= 0xBF;
        }
        if (ok)
        {
            out.append(text, i, len);
            i += len;
            continue;
        }
        out += static_cast<char>(0xEF);
        out += static_cast<char>(0xBF);
        out += static_cast<char>(0xBD);
        replaced = true;
        ++i;
    }
    if (replaced)
        text.swap(out);
    return replaced;
}

// One subtitle as text: its UTF-8 text with the lines joined by a line feed and every tag as written,
// at its start, with its length as the duration. Times go through whole milliseconds, which is what
// SRT holds, so they arrive exactly.
int SRTStreamReader::readTextPacket(AVPacket& avPacket)
{
    avPacket.codec = this;
    avPacket.codecID = CODEC_S_SRT;
    avPacket.stream_index = m_streamIndex;
    avPacket.data = nullptr;
    avPacket.size = 0;
    avPacket.flags = 0;
    avPacket.duration = 0;
    for (;;)
    {
        while (m_textCues.empty())
            if (!readNextCue())
                return NEED_MORE_DATA;
        TextCue cue = std::move(m_textCues.front());
        m_textCues.pop_front();
        const int64_t inMs = llround(cue.inTime * 1000.0);
        const int64_t outMs = llround(cue.outTime * 1000.0);
        // A subtitle that ends when it starts, or earlier, is never on screen. Written without a
        // duration it would mean the opposite, shown until the next one, so it is left out.
        if (!cue.untilNext && outMs <= inMs)
        {
            if (m_lengthlessCues++ == 0)
                LTRACE(LT_WARN, 2,
                       "Warning: track " << m_streamIndex << ": the subtitle at " << floatToTime(cue.inTime, ',')
                                         << " ends when it starts or earlier. It is never shown, so it is left out.");
            continue;
        }
        if (makeValidUtf8(cue.text) && m_invalidUtf8Cues++ == 0)
            LTRACE(LT_WARN, 2,
                   "Warning: track " << m_streamIndex << ": the subtitle at " << floatToTime(cue.inTime, ',')
                                     << " holds bytes that are not valid UTF-8, written as the replacement "
                                        "character U+FFFD. Save the file as UTF-8 to keep every character.");
        m_textPacket.assign(cue.text.begin(), cue.text.end());
        avPacket.data = m_textPacket.data();
        avPacket.size = static_cast<int>(m_textPacket.size());
        avPacket.pts = avPacket.dts = inMs * INTERNAL_PTS_PER_MS;
        // A subtitle without an end is shown until the next one: no duration
        avPacket.duration = cue.untilNext ? 0 : (outMs - inMs) * INTERNAL_PTS_PER_MS;
        avPacket.flags = AVPacket::IS_IFRAME;
        return 0;
    }
}

int SRTStreamReader::readPacket(AVPacket& avPacket)
{
    if (m_keepText)
        return readTextPacket(avPacket);
    const int rez = m_dstSubCodec->readPacket(avPacket);
    if (rez == NEED_MORE_DATA)
    {
        uint32_t renderedLen;
        uint8_t* renderedBuffer = renderNextMessage(renderedLen);
        if (renderedBuffer)
        {
            m_dstSubCodec->setBuffer(renderedBuffer - MAX_AV_PACKET_SIZE, renderedLen,
                                     m_lastBlock && m_sourceText.empty());
            return m_dstSubCodec->readPacket(avPacket);
        }
        return NEED_MORE_DATA;
    }
    return rez;
}

uint8_t* SRTStreamReader::renderNextMessage(uint32_t& renderedLen)
{
    // Keep reading cues until something can be drawn. Returning with complete cues still waiting
    // would ask for more data, and at the end of the file there is none: they would be lost.
    for (;;)
    {
        while (m_toDraw.empty())
        {
            if (!readNextCue())
            {
                // The file is finished, so whatever is held is complete
                if (m_lastBlock && m_sourceText.empty())
                    flushCueGroup();
                break;
            }
        }
        if (m_toDraw.empty())
            return nullptr;
        // A piece that draws nothing, such as a cue of tags only, is passed over
        if (uint8_t* rez = drawNext(renderedLen))
            return rez;
    }
}

// Parses one cue and hands it to addCue. Returns false when the next cue is not complete yet.
bool SRTStreamReader::readNextCue()
{
    if (m_sourceText.empty())
    {
        // The end of the file usually arrives as a call of its own, after the last lines. The last
        // subtitle is then complete but still waiting for the blank line that would end it, and it
        // was never drawn, because this returned before looking.
        if (m_lastBlock && m_state == ParseState::PARSE_TEXT && !m_renderedText.empty())
        {
            m_state = ParseState::PARSE_FIRST_LINE;
            addCue(m_renderedText, m_inTime, m_outTime);
            m_renderedText.clear();
            return true;
        }
        return false;
    }
    if (m_state == ParseState::PARSE_FIRST_LINE)
    {
        while (!m_sourceText.empty() && m_sourceText.front().empty())
        {
            m_sourceText.pop_front();  // delete empty lines before message
            m_processedSize += m_origSize.front();
            m_origSize.pop();
        }
        if (m_sourceText.empty())
            return false;
        m_state = ParseState::PARSE_TIME;
        bool isNUmber = true;
        {
            for (const auto& c : m_sourceText.front())
                if (!(c >= '0' && c <= '9') && c != ' ')
                {
                    isNUmber = false;
                    break;
                }
        }
        if (isNUmber)
        {
            m_sourceText.pop_front();
            m_processedSize += m_origSize.front();
            m_origSize.pop();
            if (m_sourceText.empty())
                return false;
        }
    }
    if (m_state == ParseState::PARSE_TIME)
    {
        if (!parseTime(m_sourceText.front()))
        {
            // Say WHICH track and WHICH file, and where in it. A demux of thirty two subtitle
            // tracks used to end with nothing but the offending text, and when that text is a
            // blank line the message quoted an empty string and named nothing at all.
            const int64_t lineNumber = m_linesRead - static_cast<int64_t>(m_sourceText.size()) + 1;
            std::ostringstream what;
            what << "Invalid SRT format in track " << m_streamIndex;
            if (!m_streamName.empty())
                what << " (" << m_streamName << ")";
            what << ", line " << lineNumber << ": expected a timing line and found ";
            if (m_sourceText.front().empty())
                what << "a blank one.";
            else
                what << "\"" << m_sourceText.front().c_str() << "\".";
            what << " A timing line looks like 00:00:20,000 --> 00:00:24,400";
            THROW(ERR_COMMON, what.str())
        }
        m_state = ParseState::PARSE_TEXT;
        m_sourceText.pop_front();
        m_processedSize += m_origSize.front();
        m_origSize.pop();
        if (m_sourceText.empty())
            return false;
    }

    for (;;)
    {
        while (!m_sourceText.empty() && !m_sourceText.front().empty())
        {
            if (!m_renderedText.empty())
                m_renderedText += '\n';
            m_renderedText += m_sourceText.front();
            m_sourceText.pop_front();
            m_processedSize += m_origSize.front();
            m_origSize.pop();
        }
        if (m_sourceText.empty())
            break;
        // A blank line. SRT has no way to write an empty line inside a subtitle, and files that
        // contain one anyway are common: what follows it is then neither a counter nor a timing
        // line. That used to end the whole mux with a parse error. The blank line ends the
        // subtitle only when a new one follows it; otherwise it belongs to the text.
        const int ends = blankLineEndsCue();
        if (ends < 0)
            return false;  // the next lines have not arrived yet
        if (ends > 0)
            break;
        while (!m_sourceText.empty() && m_sourceText.front().empty())
        {
            m_renderedText += '\n';
            m_sourceText.pop_front();
            m_processedSize += m_origSize.front();
            m_origSize.pop();
        }
    }

    if (m_sourceText.empty())
    {
        if (m_lastBlock && !m_renderedText.empty())
        {
            // The last subtitle of a file with no blank line after it. Its text used to be cleared
            // before it was drawn, so it was drawn empty and never appeared.
            m_state = ParseState::PARSE_FIRST_LINE;
            addCue(m_renderedText, m_inTime, m_outTime);
            m_renderedText.clear();
            return true;
        }
        return false;
    }
    m_sourceText.pop_front();  // delete empty line (messages separator)
    m_processedSize += m_origSize.front();
    m_origSize.pop();
    addCue(m_renderedText, m_inTime, m_outTime);
    m_state = ParseState::PARSE_FIRST_LINE;
    m_renderedText.clear();
    return true;
}

// Closing tags for the style tags a cue leaves open, so that a cue drawn below it in the same
// picture does not inherit its italic, bold or colour. Only used when cues are stacked.
static std::string closeOpenTags(const std::string& text)
{
    static const char* const KNOWN[] = {"i", "italic", "b", "bold", "u", "underline", "strike", "f", "force", "font"};
    std::vector<std::string> open;
    size_t pos = 0;
    while ((pos = text.find('<', pos)) != std::string::npos)
    {
        const size_t end = text.find('>', pos);
        if (end == std::string::npos)
            break;
        std::string tag = text.substr(pos + 1, end - pos - 1);
        pos = end + 1;
        const bool closing = !tag.empty() && tag[0] == '/';
        if (closing)
            tag.erase(0, 1);
        tag = tag.substr(0, tag.find_first_of(" \t="));
        for (char& c : tag) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (std::find(std::begin(KNOWN), std::end(KNOWN), tag) == std::end(KNOWN))
            continue;
        if (!closing)
            open.push_back(tag);
        else if (const auto it = std::find(open.rbegin(), open.rend(), tag); it != open.rend())
            open.erase(std::next(it).base());
    }
    std::string closers;
    for (auto it = open.rbegin(); it != open.rend(); ++it) closers += "</" + *it + ">";
    return closers;
}

// Whether the text has anything to show once its tags are taken out. A cue such as "<i></i>" or one
// of spaces draws nothing; in a group it would only add an empty line that pushes the others up.
static bool hasVisibleText(const std::string& text)
{
    bool inTag = false;
    for (size_t i = 0; i < text.size(); ++i)
    {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '<')
            inTag = true;
        else if (c == '>' && inTag)
            inTag = false;
        else if (inTag || c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        else if (c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xA0)
            ++i;  // a no-break space
        else
            return true;
    }
    return false;
}

void SRTStreamReader::addCue(const std::string& text, const double inTime, const double outTime)
{
    // Kept as text, each subtitle goes out as it was written, overlaps and all: Matroska carries
    // subtitles that overlap as they are, so nothing is grouped or drawn. A subtitle with no text
    // at all has nothing to carry.
    if (m_keepText)
    {
        if (!text.empty())
            m_textCues.push_back(TextCue{text, inTime, outTime, m_untilNext});
        return;
    }
    // Each cue used to be drawn as a picture of its own that cleared the screen at its end. Two cues
    // that overlap then wrote their pictures out of time order, and the first one's end cleared
    // the second while it was still due.
    //
    // Only a subtitle FILE is grouped. It is read ahead on its own, so holding a cue until the next
    // one shows whether they overlap costs nothing. From a container the next cue may be minutes
    // away in the source, and holding the cue would deliver it late: there each cue is drawn at
    // once, as before.
    // A timing line without an end: the subtitle stays until the next one replaces it, so it is
    // drawn without the composition that would clear it. The end given to the drawing only bounds
    // the fade in.
    if (m_untilNext)
    {
        flushCueGroup();
        if (hasVisibleText(text))
            m_toDraw.push_back(Piece{text, inTime, inTime + 3600.0, false});
        return;
    }
    const Cue cue{text, inTime, outTime, m_srtRender->alignToGrid(inTime), m_srtRender->alignToGrid(outTime)};
    // Overlaps are judged on the frame grid, where the cues are drawn: two cues that touch within
    // one frame, which converted files are full of, are not an overlap. A cue with no text or no
    // length on the grid takes no part in a group.
    const bool drawable = hasVisibleText(text) && cue.outFrame > cue.inFrame;
    const bool joins =
        m_containerType == ContainerType::ctNone && !m_cueGroup.empty() && drawable && cue.inFrame < m_cueGroupEnd;
    if (!joins)
    {
        if (!m_cueGroup.empty() && cue.inFrame < m_cueGroupEnd)
            return;  // no text or no length, inside a group: nothing to add to it
        flushCueGroup();
    }
    m_cueGroupEnd = m_cueGroup.empty() ? cue.outFrame : (std::max)(m_cueGroupEnd, cue.outFrame);
    m_cueGroup.push_back(cue);
    if (m_containerType != ContainerType::ctNone || !drawable)
        flushCueGroup();
}

void SRTStreamReader::flushCueGroup()
{
    if (m_cueGroup.size() == 1)
    {
        // A cue that overlaps nothing is drawn exactly as it always was, from its times as read
        const Cue& c = m_cueGroup.front();
        m_toDraw.push_back(Piece{c.text, c.inTime, c.outTime, true});
    }
    else if (m_cueGroup.size() > 1)
    {
        // A sweep over the starts and ends. Up to MAX_STACKED cues are shown at once, the ones that
        // started last: a cue with a mistyped end time would otherwise stay on top of everything
        // that follows it, and a damaged file could make one picture of thousands of lines.
        constexpr size_t MAX_STACKED = 4;
        const size_t firstPiece = m_toDraw.size();
        std::vector<double> bounds;
        for (const Cue& c : m_cueGroup)
        {
            bounds.push_back(c.inFrame);
            bounds.push_back(c.outFrame);
        }
        std::sort(bounds.begin(), bounds.end());
        bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());
        std::vector<size_t> byStart(m_cueGroup.size());
        std::vector<size_t> byEnd(m_cueGroup.size());
        for (size_t i = 0; i < m_cueGroup.size(); ++i) byStart[i] = byEnd[i] = i;
        std::stable_sort(byStart.begin(), byStart.end(), [this](const size_t a, const size_t b)
                         { return m_cueGroup[a].inFrame < m_cueGroup[b].inFrame; });
        std::stable_sort(byEnd.begin(), byEnd.end(), [this](const size_t a, const size_t b)
                         { return m_cueGroup[a].outFrame < m_cueGroup[b].outFrame; });
        std::set<std::pair<double, size_t>> active;  // ordered by start, then by place in the file
        size_t nextStart = 0;
        size_t nextEnd = 0;
        bool capped = false;
        for (size_t k = 0; k + 1 < bounds.size(); ++k)
        {
            const double t = bounds[k];
            while (nextEnd < byEnd.size() && m_cueGroup[byEnd[nextEnd]].outFrame <= t)
            {
                active.erase({m_cueGroup[byEnd[nextEnd]].inFrame, byEnd[nextEnd]});
                ++nextEnd;
            }
            while (nextStart < byStart.size() && m_cueGroup[byStart[nextStart]].inFrame <= t)
            {
                if (m_cueGroup[byStart[nextStart]].outFrame > t)
                    active.insert({m_cueGroup[byStart[nextStart]].inFrame, byStart[nextStart]});
                ++nextStart;
            }
            if (active.empty())
                continue;
            if (active.size() > MAX_STACKED && !capped)
            {
                capped = true;
                LTRACE(LT_WARN, 2,
                       "Warning: more than " << MAX_STACKED << " subtitles of track " << m_streamIndex << " overlap at "
                                             << t << " s. Only the ones that started last are drawn while they do.");
            }
            auto first = active.begin();
            if (active.size() > MAX_STACKED)
                std::advance(first, active.size() - MAX_STACKED);
            std::string text;
            for (auto it = first; it != active.end(); ++it)
            {
                const std::string& part = m_cueGroup[it->second].text;
                if (!text.empty())
                    text += '\n';
                text += part + closeOpenTags(part);
            }
            // A boundary that changes nothing visible, such as the end of a cue that was not shown
            // because more recent ones filled the picture, extends the piece before it
            if (m_toDraw.size() > firstPiece && m_toDraw.back().outTime == t && m_toDraw.back().text == text)
            {
                m_toDraw.back().outTime = bounds[k + 1];
                continue;
            }
            m_toDraw.push_back(Piece{std::move(text), t, bounds[k + 1], false});
        }
        // The screen is cleared only where nothing follows a piece; elsewhere the next piece
        // replaces it at the same moment
        for (size_t i = firstPiece; i < m_toDraw.size(); ++i)
            if (!m_toDraw[i].hide && (i + 1 == m_toDraw.size() || m_toDraw[i + 1].inTime != m_toDraw[i].outTime))
                m_toDraw[i].hide = true;
    }
    m_cueGroup.clear();
}

uint8_t* SRTStreamReader::drawNext(uint32_t& renderedLen)
{
    Piece piece = std::move(m_toDraw.front());
    m_toDraw.pop_front();
    // Fades belong to the edges of what is on screen: none where a piece takes over from another
    text_subtitles::TextAnimation animation = m_animation;
    if (m_screenShown)
        animation.fadeInDuration = 0;
    if (!piece.hide)
        animation.fadeOutDuration = 0;
    if (uint8_t* rez =
            m_srtRender->doConvert(piece.text, animation, piece.inTime, piece.outTime, renderedLen, piece.hide))
    {
        m_screenShown = !piece.hide;
        return rez;
    }
    // This piece draws nothing. If the one before it is still on screen, it has to go now, or it
    // would stay until whatever is drawn next.
    if (m_screenShown)
    {
        m_screenShown = false;
        return m_srtRender->doClear(piece.inTime, renderedLen);
    }
    return nullptr;
}

// Whether the blank line at the front ends the subtitle being read: 1 when what follows the blank
// lines is a timing line, or a counter followed by one, or the end of the file; 0 when it is more
// text of the same subtitle; -1 when the lines that decide it have not arrived yet.
int SRTStreamReader::blankLineEndsCue() const
{
    size_t i = 0;
    while (i < m_sourceText.size() && m_sourceText[i].empty()) ++i;
    // From a container each cue arrives whole, as one block of its own that ends with its blank line,
    // and the next cue comes only with the next block, which may be minutes away in the source.
    // Waiting for it delivered every cue late, and the last one never, because a track inside a
    // container is not always told when its data has ended. So there, a blank line with nothing
    // after it ends the cue. An empty line inside a cue still has its cue's later lines behind it.
    if (i == m_sourceText.size())
        return m_lastBlock || m_containerType != ContainerType::ctNone ? 1 : -1;
    const std::string& next = m_sourceText[i];
    if (next.find("-->") != std::string::npos)
        return 1;
    const bool isCounter =
        std::all_of(next.begin(), next.end(), [](const char c) { return (c >= '0' && c <= '9') || c == ' '; });
    if (!isCounter)
        return 0;
    if (i + 1 == m_sourceText.size())
        return m_lastBlock ? 1 : -1;
    return m_sourceText[i + 1].find("-->") != std::string::npos ? 1 : 0;
}

bool SRTStreamReader::parseTime(const string& text)
{
    // length() - 2 is unsigned, so a line shorter than the arrow it is looking for wrapped the
    // bound to SIZE_MAX and the loop walked off the end of the string. A blank line where a
    // timing line belongs is exactly that case, and it is what a damaged subtitle file gives.
    for (size_t i = 0; i + 2 < text.length(); i++)
    {
        if (text[i] == '-' && text[i + 1] == '-' && text[i + 2] == '>')
        {
            string first = trimStr(text.substr(0, i));
            string second = trimStr(text.substr(i + 3, text.length() - i - 3));
            // Some writers put display coordinates after the end time, "X1:100 X2:600 Y1:50 Y2:100".
            // The end time is read by splitting at colons from the right, so those numbers were read
            // as the time: a subtitle due to end at 3 s ended at 600 hours. Only the first word after
            // the arrow is the time.
            if (const size_t gap = second.find_first_of(" \t"); gap != string::npos)
                second.resize(gap);
            for (char& c : first)
            {
                if (c == ',')
                    c = '.';
            }
            for (char& c : second)
            {
                if (c == ',')
                    c = '.';
            }

            m_inTime = timeToFloat(first);
            m_outTime = timeToFloat(second);
            m_untilNext = second.empty();
            return true;
        }
    }
    return false;
}

// ReSharper disable once CppMemberFunctionMayBeStatic
CheckStreamRez SRTStreamReader::checkStream(uint8_t* buffer, const int len, const ContainerType containerType,
                                            const int containerDataType, int containerStreamIndex)
{
    CheckStreamRez rez;
    if (((containerType == ContainerType::ctMKV || containerType == ContainerType::ctMOV) &&
         containerDataType == TRACKTYPE_SRT) ||
        containerType == ContainerType::ctSRT)
    {
        rez.codecInfo = srtCodecInfo;
        rez.streamDescr = "SRT text subtitles";
        if (containerType == ContainerType::ctSRT)
            rez.lang = detectUTF8Lang(buffer, len);
    }
    return rez;
}

std::string SRTStreamReader::detectUTF8Lang(uint8_t* buffer, int len) { return ""; }
