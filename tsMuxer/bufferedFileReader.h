#ifndef BUFFERED_FILE_READER_H_
#define BUFFERED_FILE_READER_H_

#include <fs/file.h>
#include <types/types.h>

#include "bufferedReader.h"
#include "tsPacket.h"
#include "vodCoreException.h"

// A part of a play timeline (a Matroska ordered edition): the stretch of its file to play, in the file's own
// nanoseconds, and where the part starts in the timeline. startNs -1: the whole file, as a plain join plays it.
struct JoinRange
{
    int64_t startNs = -1;
    int64_t endNs = INT64_MAX;
    int64_t offsetNs = 0;
    std::string name;  // the chapter's
};

// Used to automatically switch to reading the next file in a given list
class FileListIterator final : public FileNameIterator
{
   public:
    FileListIterator() : m_index(0) {}
    ~FileListIterator() override = default;

    std::string getNextName() override { return ++m_index < m_files.size() ? m_files[m_index] : ""; }

    void addFile(const std::string& fileName) { m_files.push_back(fileName); }
    void addFile(const std::string& fileName, const JoinRange& range)
    {
        m_files.push_back(fileName);
        m_ranges.push_back(range);
    }
    [[nodiscard]] const std::vector<std::string>& getFiles() const { return m_files; }
    // One for each file of a play timeline; empty for a plain join.
    [[nodiscard]] const std::vector<JoinRange>& getRanges() const { return m_ranges; }

   private:
    std::vector<std::string> m_files;
    std::vector<JoinRange> m_ranges;
    size_t m_index;
};

struct FileReaderData final : ReaderData
{
    typedef ReaderData base_class;

    FileReaderData(uint32_t blockSize, uint32_t allocSize) : m_fileHeaderSize(0) {}

    ~FileReaderData() override = default;

    int readBlock(uint8_t* buffer, const uint32_t max_size) override { return m_file.read(buffer, max_size); }

    bool openStream() override;
    bool closeStream() override { return m_file.close(); }
    bool incSeek(const int64_t offset) override { return m_file.seek(offset, File::SeekMethod::smCurrent) != -1; }

    File m_file;
    uint32_t m_fileHeaderSize;
};

class BufferedFileReader final : public BufferedReader
{
   public:
    BufferedFileReader(uint32_t blockSize, uint32_t allocSize = 0, uint32_t prereadThreshold = 0);

    bool openStream(int readerID, const char* streamName, int pid = 0, const CodecInfo* codecInfo = nullptr) override;
    bool gotoByte(int readerID, int64_t seekDist) override;

   protected:
    ReaderData* intCreateReader() override { return new FileReaderData(m_blockSize, m_allocSize); }
};

#endif
