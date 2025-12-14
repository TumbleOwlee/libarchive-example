#include <archive.h>
#include <archive_entry.h>
#include <expected>
#include <fstream>
#include <iostream>
#include <memory>
#include <queue>
#include <string>

namespace compression {

enum class Error {
    InitFailed = 0,
    SetFormatFailed,
    SetCompressionFailed,
    OpenFailed,
    WriteFailed,
    StatFailed,
    FileChanged,
};

enum class Mode {
    NonBlock,
    Block,
};

enum class State {
    InProgress,
    Finished,
};

class Writer {
public:
    template <typename T>
    using Result = std::expected<T, Error>;
    using Pointer = std::unique_ptr<Writer>;

    ~Writer() = default;

    static auto open(std::string filename, size_t bufferSize = 512) -> Result<Pointer> {
        auto writer = std::unique_ptr<Writer>(new Writer(bufferSize));
        auto res = writer->open();

        if (!res) {
            return std::unexpected<Error>(res.error());
        }

#if USE_ZIP
        filename += ".zip";
#else
        filename += ".tar.lz4";
#endif

        if (archive_write_open_filename(writer->archive.get(), filename.c_str()) != ARCHIVE_OK) {
            return std::unexpected(Error::OpenFailed);
        }

        return writer;
    }

    static auto open(archive_open_callback open, archive_write_callback write, archive_close_callback close,
                     archive_free_callback free, void *userdata = nullptr, size_t bufferSize = 512) -> Result<Pointer> {
        auto writer = std::unique_ptr<Writer>(new Writer(bufferSize));
        auto res = writer->open();

        if (!res) {
            return std::unexpected<Error>(res.error());
        }

        if (archive_write_open2(writer->archive.get(), userdata, open, write, close, free) != ARCHIVE_OK) {
            return std::unexpected(Error::OpenFailed);
        }

        return writer;
    }

    auto add_file(std::string filename) -> bool {
        struct stat64 stat;
        if (0 == lstat64(filename.c_str(), &stat)) {
            files.push(filename);
            return true;
        } else {
            return false;
        }
    }

    auto write(Mode mode = Mode::Block) -> Result<State> {
        if (!input.is_open() && files.empty()) {
            return State::Finished;
        }

        do {
            // Open file if not already opened
            if (!input.is_open()) {
                std::string file = files.front();
                input.open(file.c_str(), std::ios_base::in);
                files.pop();

                if (!input.is_open()) {
                    return std::unexpected(Error::OpenFailed);
                }

                struct stat64 stat;
                if (lstat64(file.c_str(), &stat) < 0) {
                    return std::unexpected(Error::StatFailed);
                }

                entry.header.reset(archive_entry_new());
                entry.remainingSize = stat.st_size;
                entry.totalSize = stat.st_size;

                archive_entry_set_pathname(entry.header.get(), file.c_str());
                archive_entry_set_size(entry.header.get(), stat.st_size);
                archive_entry_set_filetype(entry.header.get(), AE_IFREG);
                archive_entry_set_uid(entry.header.get(), 1000);
                archive_entry_set_gid(entry.header.get(), 1000);
                archive_entry_set_mode(entry.header.get(), S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

                auto res = archive_write_header(archive.get(), entry.header.get());

                if (res != ARCHIVE_OK) {
                    return std::unexpected(Error::WriteFailed);
                }
            }

            // Write until predefined size is written
            if (entry.remainingSize > 0) {
                // Read from file until buffer is full
                if (buffer.size > buffer.filled && !input.eof()) {
                    buffer.filled += input.readsome(&buffer.data.get()[buffer.filled], buffer.size - buffer.filled);
                }

                // Write from buffer into archive if data is available
                if (buffer.filled - buffer.extracted > 0) {
                    auto written = archive_write_data(archive.get(), &buffer.data.get()[buffer.extracted],
                                                      buffer.filled - buffer.extracted);
                    if (written < 0) {
                        return std::unexpected(Error::WriteFailed);
                    }

                    entry.remainingSize -= written;
                    buffer.extracted += written;
                }

                // Reset buffer if all is written
                if (buffer.filled <= buffer.extracted) {
                    buffer.filled = 0;
                    buffer.extracted = 0;
                }
            }

            if (input.eof() && entry.remainingSize > 0) {
                return std::unexpected(Error::FileChanged);
            }

            // Reset entry for next file
            if (entry.remainingSize <= 0 || input.eof()) {
                archive_write_finish_entry(archive.get());
                entry.header.reset();
                entry.remainingSize = 0;
                entry.totalSize = 0;
                input.close();
            }
        } while (mode == Mode::Block &&
                 ((input.is_open() && (!input.eof() || buffer.extracted >= buffer.filled)) || !files.empty()));

        return ((input.is_open() && (!input.eof() || buffer.extracted >= buffer.filled)) || !files.empty())
                   ? State::InProgress
                   : State::Finished;
    }

    auto close() -> void {
        archive.reset();
        std::queue<std::string> q;
        std::swap(q, files);
        if (input.is_open()) {
            input.close();
        }
    }

private:
    struct Buffer {
        std::unique_ptr<char> data;
        size_t size;
        size_t filled;
        size_t extracted;

        Buffer(size_t size) : data(new char[size]), size(size) {}
        ~Buffer() = default;
    };

    struct ArchiveDeleter {
        auto operator()(struct archive *archive) -> void {
            if (archive != nullptr) {
                archive_write_free(archive);
            }
        }
    };

    struct EntryDeleter {
        auto operator()(struct archive_entry *entry) -> void {
            if (entry != nullptr) {
                archive_entry_free(entry);
            }
        }
    };

    struct Entry {
        std::unique_ptr<struct archive_entry, EntryDeleter> header;
        size_t totalSize;
        size_t remainingSize;
    };

    Entry entry;
    std::unique_ptr<struct archive, ArchiveDeleter> archive;
    std::queue<std::string> files;
    std::ifstream input;
    Buffer buffer;

    Writer(size_t bufferSize = 512) : buffer(bufferSize) {}

    auto open() -> Result<void> {
        archive.reset(archive_write_new());
        if (archive == nullptr) {
            return std::unexpected(Error::InitFailed);
        }

#if USE_ZIP
        return setupZip();
#else
        return setupLZ4();
#endif
    }

    auto setupLZ4() -> Result<void> {
        if (archive_write_set_format_pax(archive.get()) != ARCHIVE_OK) {
            return std::unexpected(Error::SetFormatFailed);
        }

        if (archive_write_add_filter_lz4(archive.get()) != ARCHIVE_OK) {
            return std::unexpected(Error::SetCompressionFailed);
        }

        if (archive_write_set_bytes_per_block(archive.get(), buffer.size) != ARCHIVE_OK) {
            return std::unexpected(Error::SetCompressionFailed);
        }

        return std::expected<void, Error>();
    }

    auto setupZip() -> Result<void> {
        if (archive_write_set_format_zip(archive.get()) != ARCHIVE_OK) {
            return std::unexpected(Error::SetFormatFailed);
        }

        if (archive_write_zip_set_compression_deflate(archive.get()) != ARCHIVE_OK) {
            return std::unexpected(Error::SetCompressionFailed);
        }

        if (archive_write_set_bytes_per_block(archive.get(), buffer.size) != ARCHIVE_OK) {
            return std::unexpected(Error::SetCompressionFailed);
        }

        return std::expected<void, Error>();
    }
};

} // namespace compression
