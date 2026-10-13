#ifndef BITMAP_SUBTITLES_H_
#define BITMAP_SUBTITLES_H_

#include <cstdint>
#include <memory>
#include <vector>

namespace text_subtitles
{
class TextToPGSConverter;
}

// The pictures of a bitmap subtitle format other than PGS, redrawn as PGS display sets in the form of a .sup file:
// every segment behind a "PG" header that carries its own times. The PGS reader takes them from there as it takes
// any PGS track, for a transport stream, a disc or a Matroska output alike.
class BitmapToPgs
{
   public:
    BitmapToPgs();
    ~BitmapToPgs();
    // The size of the pictures and the frame rate the display sets are timed to.
    void setCanvas(uint16_t width, uint16_t height, double fps);
    [[nodiscard]] uint16_t width() const { return m_width; }
    [[nodiscard]] uint16_t height() const { return m_height; }
    // The picture, a 32 bit BGRA value per pixel and 0 where it is transparent.
    uint32_t* pixels() { return m_canvas.data(); }
    void clearCanvas();
    // The display sets that show the picture from showTime on (internal clock). First the end of the picture before,
    // where it ends before this one starts; a picture that starts first replaces it. The end of this one, endTime,
    // is held until the next picture or finish(), and is -1 when it stays until the next. A picture with nothing
    // in it ends what is shown.
    void addPicture(int64_t showTime, int64_t endTime, bool forced, std::vector<uint8_t>& out);
    // The end that is still held, at the end of the track.
    void finish(std::vector<uint8_t>& out);

   private:
    std::unique_ptr<text_subtitles::TextToPGSConverter> m_pgs;
    std::vector<uint32_t> m_canvas;
    uint16_t m_width = 0;
    uint16_t m_height = 0;
    bool m_shown = false;    // a picture is on the screen
    int64_t m_heldEnd = -1;  // when it goes, internal clock; -1 when it stays until the next
    bool m_warnedColours = false;
    void addClear(double atTimeSec, std::vector<uint8_t>& out);
};

// The frame rate a PGS stream can be marked with that is nearest to fps, or 0 when none is near.
double nearestPgsFrameRate(double fps);

// The subpictures of a DVD subtitle track, S_VOBSUB in Matroska: the CodecPrivate is the text of the .idx file, with
// the picture size and the 16 colours, and every block is one subpicture unit.
class VobSubDecoder
{
   public:
    // fps: the frame rate of the file's video, 0 when the file has none.
    VobSubDecoder(const uint8_t* idx, int idxSize, double fps);
    // One subpicture unit, from a block at pts (internal clock) that lasts duration, or -1 when it does not say.
    void decode(const uint8_t* spu, int size, int64_t pts, int64_t duration, std::vector<uint8_t>& out);
    void finish(std::vector<uint8_t>& out) { m_pgs.finish(out); }
    // The frame rate was not known from a video track and was taken from the picture height.
    [[nodiscard]] bool rateGuessed() const { return m_rateGuessed; }

   private:
    BitmapToPgs m_pgs;
    uint32_t m_palette[16]{};  // 0xRRGGBB
    bool m_rateGuessed = false;
    bool m_warned = false;
};

#endif
