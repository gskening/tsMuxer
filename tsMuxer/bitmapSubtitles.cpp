#include "bitmapSubtitles.h"

#include <fs/systemlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include "textSubtitles.h"
#include "vod_common.h"

using text_subtitles::CompositionMode;
using text_subtitles::PIXEL_COMPOSITION_RATE;
using text_subtitles::PIXEL_DECODING_RATE;
using text_subtitles::TextToPGSConverter;

double nearestPgsFrameRate(const double fps)
{
    static constexpr double RATES[] = {24000.0 / 1001, 24, 25, 30000.0 / 1001, 30, 50, 60000.0 / 1001, 60};
    for (const double rate : RATES)
        if (fabs(fps - rate) < rate * 0.005)
            return rate;
    return 0;
}

BitmapToPgs::BitmapToPgs() : m_pgs(std::make_unique<TextToPGSConverter>(false)) {}

BitmapToPgs::~BitmapToPgs() = default;

void BitmapToPgs::setCanvas(const uint16_t width, const uint16_t height, const double fps)
{
    m_width = width;
    m_height = height;
    m_canvas.assign(static_cast<size_t>(width) * height, 0);
    m_pgs->setCanvas(width, height, fps);
    m_pgs->setImageBuffer(reinterpret_cast<uint8_t*>(m_canvas.data()));
}

void BitmapToPgs::clearCanvas() { std::fill(m_canvas.begin(), m_canvas.end(), 0); }

void BitmapToPgs::addClear(const double atTimeSec, std::vector<uint8_t>& out)
{
    uint32_t len = 0;
    const uint8_t* data = m_pgs->doClear(atTimeSec, len);
    out.insert(out.end(), data, data + len);
    m_shown = false;
}

void BitmapToPgs::addPicture(const int64_t showTime, const int64_t endTime, const bool forced,
                             std::vector<uint8_t>& out)
{
    const double inSec = m_pgs->alignToGrid(static_cast<double>(showTime) / INTERNAL_PTS_FREQ);
    // The end of the picture before, when it comes before this one. When it does not, or when the two fall on the
    // same frame, the new picture replaces the old one on the screen, as a text subtitle does that follows the one
    // before without a gap.
    if (m_shown && m_heldEnd >= 0)
    {
        const double outSec = m_pgs->alignToGrid(static_cast<double>(m_heldEnd) / INTERNAL_PTS_FREQ);
        if (outSec < inSec)
            addClear(outSec, out);
    }
    m_heldEnd = -1;

    // Fewer colours until the picture packs: the same steps as for a text subtitle.
    uint32_t mask = 0;
    int step = 0;
    while (!m_pgs->rlePack(mask))
    {
        if (++step > 32)
        {
            if (!m_warnedColours)
                LTRACE(LT_WARN, 2,
                       "Warning: a subtitle picture at " << floatToTime(inSec)
                                                         << " could not be packed as PGS and was left out.");
            m_warnedColours = true;
            return;
        }
        const auto tmp = reinterpret_cast<uint8_t*>(&mask);
        const int idx = (step - 1) % 4;
        tmp[idx] <<= 1;
        tmp[idx]++;
    }
    if (m_pgs->m_rleLen == 0)
    {
        // nothing to show: the picture empties the screen
        if (m_shown)
            addClear(inSec, out);
        return;
    }

    // As for a text subtitle (TextToPGSConverter::doConvert): the presentation, the window, the palette and the
    // object, each with the times a player needs to decode and draw it, then the end of the display set. The window
    // and the object are where the picture's lines are, across the full width.
    const uint16_t top = m_pgs->minLine();
    const uint16_t lines = m_pgs->renderedHeight();
    const double decodedObjectSize = static_cast<double>(lines) * m_width;
    const double compositionDecodeTime = 90000.0 * decodedObjectSize / PIXEL_DECODING_RATE + 0.999;
    const double windowsTransferTime = 90000.0 * decodedObjectSize / PIXEL_COMPOSITION_RATE + 0.999;
    const double planeInitializationTime =
        90000.0 * (static_cast<double>(m_width) * m_height) / PIXEL_COMPOSITION_RATE + 0.999;
    const double presentationDtsDelta = planeInitializationTime + windowsTransferTime;
    m_pgs->m_lastWindowTop = top;
    m_pgs->m_lastWindowHeight = lines;
    m_pgs->m_lastWindowsTransferTime = windowsTransferTime;

    const double inPts = inSec * 90000.0;
    const int64_t pts = llround(inPts);
    const int64_t dts = llround(inPts - presentationDtsDelta);
    const int64_t odfPts = llround(inPts - presentationDtsDelta + compositionDecodeTime);
    m_pgs->palette_update_flag = false;
    m_pgs->m_paletteID = 0;
    m_pgs->m_paletteVersion = 0;

    // A display set needs at most its picture packed, which rlePack keeps within (width + 16) bytes a line, and the
    // headers of its segments.
    std::vector<uint8_t> buf(static_cast<size_t>(m_pgs->m_rleLen) + (m_pgs->m_rleLen / 65000 + 1) * 32 + 2048);
    uint8_t* cur = buf.data();
    cur += m_pgs->composePresentationSegment(cur, CompositionMode::Start, pts, dts, top, true, forced);
    cur += m_pgs->composeWindowDefinition(cur, llround(inPts - windowsTransferTime), dts, top, lines);
    cur += m_pgs->composePaletteDefinition(m_pgs->m_paletteByColor, cur, dts, dts);
    cur += m_pgs->composeObjectDefinition(cur, odfPts, dts, m_pgs->minLine(), m_pgs->maxLine(), true);
    cur += m_pgs->composeEnd(cur, odfPts, odfPts);
    out.insert(out.end(), buf.data(), cur);
    m_shown = true;
    m_heldEnd = endTime;
}

void BitmapToPgs::finish(std::vector<uint8_t>& out)
{
    if (m_shown && m_heldEnd >= 0)
        addClear(m_pgs->alignToGrid(static_cast<double>(m_heldEnd) / INTERNAL_PTS_FREQ), out);
    m_heldEnd = -1;
}

// ------------ DVD subpictures ---------------

VobSubDecoder::VobSubDecoder(const uint8_t* idx, const int idxSize, double fps)
{
    // The .idx text: "size: 720x576" and "palette: rrggbb, rrggbb, ..." are the lines that matter here.
    int width = 720;
    int height = 576;
    bool haveSize = false;
    std::istringstream lines(std::string(reinterpret_cast<const char*>(idx), idx ? static_cast<size_t>(idxSize) : 0));
    std::string line;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("size:", 0) == 0)
        {
            int w = 0, h = 0;
            if (sscanf(line.c_str() + 5, " %dx%d", &w, &h) == 2 && w > 0 && h > 0 && w <= 1920 && h <= 1080)
            {
                width = w;
                height = h;
                haveSize = true;
            }
        }
        else if (line.rfind("palette:", 0) == 0)
        {
            std::istringstream colours(line.substr(8));
            std::string colour;
            for (int i = 0; i < 16 && std::getline(colours, colour, ','); ++i)
                m_palette[i] = static_cast<uint32_t>(strtoul(colour.c_str(), nullptr, 16)) & 0xFFFFFF;
        }
    }
    if (!haveSize)
        LTRACE(LT_WARN, 2, "Warning: the DVD subtitle track gives no picture size; 720x576 is used.");

    // The display sets are marked with the video's frame rate. Without a video track in the file, the rate of the
    // DVD standard for the picture height: 25 for 576 lines, 29.97 for 480.
    fps = nearestPgsFrameRate(fps);
    if (fps == 0)
    {
        fps = height == 576 ? 25.0 : 30000.0 / 1001;
        m_rateGuessed = true;
    }
    m_pgs.setCanvas(static_cast<uint16_t>(width), static_cast<uint16_t>(height), fps);
}

namespace
{
// The pixels of one field of a subpicture: run lengths in 4 bit steps (DVD-Video, subpicture unit).
class NibbleReader
{
   public:
    NibbleReader(const uint8_t* data, const int start, const int end) : m_data(data), m_pos(start * 2), m_end(end * 2)
    {
    }
    [[nodiscard]] bool atEnd() const { return m_pos >= m_end; }
    int nibble()
    {
        if (m_pos >= m_end)
            return 0;
        const int v = m_pos & 1 ? m_data[m_pos / 2] & 0x0F : m_data[m_pos / 2] >> 4;
        ++m_pos;
        return v;
    }
    // 1 to 4 nibbles: the run length in the upper bits, the colour index in the lowest 2. A length of 0 runs to
    // the end of the line.
    int code()
    {
        int v = nibble();
        if (v < 0x4)
        {
            v = v << 4 | nibble();
            if (v < 0x10)
            {
                v = v << 4 | nibble();
                if (v < 0x40)
                    v = v << 4 | nibble();
            }
        }
        return v;
    }
    void alignToByte() { m_pos = (m_pos + 1) & ~1; }

   private:
    const uint8_t* m_data;
    int m_pos;  // in nibbles
    int m_end;
};
}  // namespace

void VobSubDecoder::decode(const uint8_t* spu, const int size, const int64_t pts, const int64_t duration,
                           std::vector<uint8_t>& out)
{
    const auto bad = [this]()
    {
        if (!m_warned)
            LTRACE(LT_WARN, 2, "Warning: a damaged DVD subtitle picture was left out.");
        m_warned = true;
    };
    if (size < 4)
        return bad();
    const int unitSize = (std::min)(static_cast<int>(AV_RB16(spu)), size);
    const int firstSequence = AV_RB16(spu + 2);
    if (firstSequence < 4 || firstSequence + 4 > unitSize)
        return bad();

    // The control sequences: each starts after a delay from the unit's time, in units of 1024 ticks of 90 kHz, and
    // holds commands until 0xFF.
    int startDate = -1, stopDate = -1;
    bool forced = false;
    int colour[4] = {0, 1, 2, 3};
    int alpha[4] = {0, 15, 15, 15};
    int x1 = 0, x2 = -1, y1 = 0, y2 = -1;
    int fieldStart[2] = {0, 0};
    int pos = firstSequence;
    for (int sequences = 0; sequences < 64 && pos + 4 <= unitSize; ++sequences)
    {
        const int date = AV_RB16(spu + pos);
        const int next = AV_RB16(spu + pos + 2);
        int p = pos + 4;
        bool end = false;
        while (!end && p < unitSize)
        {
            switch (spu[p++])
            {
            case 0x00:  // forced start of display
                forced = true;
                [[fallthrough]];
            case 0x01:  // start of display
                if (startDate < 0)
                    startDate = date;
                break;
            case 0x02:  // stop of display
                if (stopDate < 0)
                    stopDate = date;
                break;
            case 0x03:  // the palette entry of each of the 4 pixel values, the last one first
                if (p + 2 > unitSize)
                    return bad();
                colour[3] = spu[p] >> 4;
                colour[2] = spu[p] & 0x0F;
                colour[1] = spu[p + 1] >> 4;
                colour[0] = spu[p + 1] & 0x0F;
                p += 2;
                break;
            case 0x04:  // the opacity of each, 0 to 15, in the same order
                if (p + 2 > unitSize)
                    return bad();
                alpha[3] = spu[p] >> 4;
                alpha[2] = spu[p] & 0x0F;
                alpha[1] = spu[p + 1] >> 4;
                alpha[0] = spu[p + 1] & 0x0F;
                p += 2;
                break;
            case 0x05:  // the area: first and last column, first and last line, 12 bits each
                if (p + 6 > unitSize)
                    return bad();
                x1 = spu[p] << 4 | spu[p + 1] >> 4;
                x2 = (spu[p + 1] & 0x0F) << 8 | spu[p + 2];
                y1 = spu[p + 3] << 4 | spu[p + 4] >> 4;
                y2 = (spu[p + 4] & 0x0F) << 8 | spu[p + 5];
                p += 6;
                break;
            case 0x06:  // where the lines of the top and of the bottom field start
                if (p + 4 > unitSize)
                    return bad();
                fieldStart[0] = AV_RB16(spu + p);
                fieldStart[1] = AV_RB16(spu + p + 2);
                p += 4;
                break;
            case 0x07:  // colour and contrast changes within the area: its size word counts itself
            {
                if (p + 2 > unitSize || AV_RB16(spu + p) < 2)
                {
                    end = true;
                    break;
                }
                p += AV_RB16(spu + p);
                break;
            }
            default:  // 0xFF ends the sequence, and an unknown command cannot be stepped over
                end = true;
            }
        }
        if (next <= pos)
            break;  // the last sequence points to itself
        pos = next;
    }

    const auto toInternal = [](const int date)
    { return static_cast<int64_t>(date) * 1024 * INTERNAL_PTS_FREQ / 90000; };
    const int64_t showTime = pts + (startDate > 0 ? toInternal(startDate) : 0);
    int64_t endTime = -1;
    if (stopDate > (std::max)(startDate, 0))
        endTime = pts + toInternal(stopDate);
    else if (duration > 0)
        endTime = pts + duration;

    // A unit that only stops the display has nothing to draw, and its empty picture ends what is shown.
    m_pgs.clearCanvas();
    if (startDate >= 0 && x2 >= x1 && y2 >= y1 && fieldStart[0] >= 4 && fieldStart[1] >= 4)
    {
        const int w = x2 - x1 + 1;
        const int h = y2 - y1 + 1;
        uint32_t argb[4];
        for (int i = 0; i < 4; ++i)
            argb[i] = alpha[i] == 0 ? 0 : static_cast<uint32_t>(alpha[i] * 17) << 24 | m_palette[colour[i]];
        // The pixel data ends where the control sequences begin.
        NibbleReader field[2] = {NibbleReader(spu, fieldStart[0], firstSequence),
                                 NibbleReader(spu, fieldStart[1], firstSequence)};
        uint32_t* canvas = m_pgs.pixels();
        for (int y = 0; y < h; ++y)
        {
            NibbleReader& reader = field[y & 1];
            const int canvasY = y1 + y;
            for (int x = 0; x < w && !reader.atEnd();)
            {
                const int code = reader.code();
                int run = code >> 2;
                if (run == 0 || run > w - x)
                    run = w - x;
                const uint32_t pixel = argb[code & 3];
                if (pixel && canvasY < m_pgs.height())
                    for (int i = 0; i < run; ++i)
                        if (x1 + x + i < m_pgs.width())
                            canvas[static_cast<size_t>(canvasY) * m_pgs.width() + x1 + x + i] = pixel;
                x += run;
            }
            reader.alignToByte();
        }
    }
    m_pgs.addPicture(showTime, endTime, forced, out);
}
