#include <archive.h>
#include <archive_entry.h>
#include <expected>
#include <fstream>
#include <iostream>
#include <memory>
#include <queue>
#include <string>

namespace compression {

/*!
 * \brief Error codes
 *
 * \details The Error codes are returned if an operation fails
 */
enum class Error {
    InitFailed = 0,
    SetFormatFailed,
    SetCompressionFailed,
    OpenFailed,
    WriteFailed,
    StatFailed,
    FileChanged,
};

/*!
 * \brief Modes of operation
 *
 * \details The two possible modes are given as non-blocking and blocking.
 *          In case of non-blocking mode, the operation will perform only a
 *          single step before returning.
 */
enum class Mode {
    NonBlock,
    Block,
};

/*!
 * \brief State of operation
 *
 * \details The state of operation is either given as in progress or finished.
 *          Any blocking call will guarantee that it's finished after a single
 *          call. A non-blocking call may return with 'InProgress' if the
 *          operation couldn't finish without blocking or if required multiple
 *          steps to fully finalize the operation.
 */
enum class State {
    InProgress,
    Finished,
};

/*!
 * \brief Writer of an archive file
 * 
 * \details The writer will compress a list of files into a single archive.
 *          Normally this operation will save around 75% of the necessary
 *          storage.
 */
class Writer {
public:
    template <typename T>
    using Result = std::expected<T, Error>;
    using Pointer = std::unique_ptr<Writer>;
    
    /*!
     * \brief Destructor of the writer
     *
     * \details The destructor will finalize the archive. If any file isn't
     *          written fully. The data will not be contained in the archive.
     */
    ~Writer() = default;

    /*!
     * \brief Open new archive to write into
     *
     * \details Create a new archive under the given filename. The given buffer
     *          buffer size will determine the block size of any compression
     *          step.
     *
     * \param filename   Output filename of the archive
     * \param bufferSize The output buffer size
     *
     * \return Result container either a reference to the writer or an error code
     */
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

    /*!
     * \brief Open new archive using custom callbacks
     *
     * \details Create a new archive using custom callbacks. The given buffer
     *          buffer size will determine the block size of any compression
     *          step.
     *
     * \param open       Custom callback to open output archive
     * \param write      Custom callback to write to output archive
     * \param close      Custom callback to close output archive
     * \param free       Custom callback to free userdata
     * \param userdata   Userdata passed to all callbacks
     * \param bufferSize The output buffer size
     *
     * \return Result container either a reference to the writer or an error code
     */
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

    /*!
     * \brief Add a file to the archive list
     *
     * \details Adds The given file to the list of files to write to the archive.
     *          It fails if the specified file doesn't exist. Returns false in 
     *          this case. Return true otherwise.
     *
     * \param filename The filename to add to the list
     *
     * \return True if files is added successfully. False otherwise.
     */
    auto add_file(std::string filename) -> bool {
        struct stat64 stat;
        if (0 == lstat64(filename.c_str(), &stat)) {
            files.push(filename);
            return true;
        } else {
            return false;
        }
    }

    /*!
     * \brief Compress and write the queued files into the output archive
     *
     * \details Based on the provided mode either everything is written into
     *          the archive or only a single step is performed. If non-blocking
     *          mode is selected, the operation has to be repeated until the 
     *          result state is given as finished.
     * 
     * \param mode Mode of operation
     *
     * \return Result with the state of operation on success, error code on failure.
     */
    auto write(Mode mode = Mode::Block) -> Result<State> {
        // Nothing to do, archive is completely written
        if (!input.is_open() && files.empty()) {
            return State::Finished;
        }

        // Repeat in blocking mode until everything is written
        do {
            // Open file if not already opened
            if (!input.is_open()) {
                std::string file = files.front();
                input.open(file.c_str(), std::ios_base::in);
                files.pop();

                // Failed to open input file
                if (!input.is_open()) {
                    return std::unexpected(Error::OpenFailed);
                }

                // Get stats of input file, especially its size
                struct stat64 stat;
                if (lstat64(file.c_str(), &stat) < 0) {
                    return std::unexpected(Error::StatFailed);
                }

                // Create new entry for the file
                entry.header.reset(archive_entry_new());

                // Save total size, init remaining size to be written
                entry.remainingSize = stat.st_size;
                entry.totalSize = stat.st_size;

                // Set entry meta information
                archive_entry_set_pathname(entry.header.get(), file.c_str());
                archive_entry_set_size(entry.header.get(), stat.st_size);
                archive_entry_set_filetype(entry.header.get(), AE_IFREG);
                archive_entry_set_uid(entry.header.get(), 1000);
                archive_entry_set_gid(entry.header.get(), 1000);
                archive_entry_set_mode(entry.header.get(), S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

                // Write header to archive
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

            // File content has changed after queuing
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

    /*!
     * \brief Close the archive
     *
     * \details Closing the archive will force the write of the end-section
     *          that depends on the used compression and output file type.
     */
    auto close() -> void {
        archive.reset();
        std::queue<std::string> q;
        std::swap(q, files);
        if (input.is_open()) {
            input.close();
        }
    }

private:
    /*!
     * \brief Input-Output buffer
     *
     * \details The buffer is utilized to read input files and push the
     *          content into the archive.
     */
    struct Buffer {
        std::unique_ptr<char> data;
        size_t size;
        size_t filled;
        size_t extracted;

        /*!
         * \brief Constructor
         *
         * \param size Size of the internal buffer
         */
        Buffer(size_t size) : data(new char[size]), size(size) {}

        /*!
         * \brief Destructor
         */
        ~Buffer() = default;
    };

    /*!
     * \brief Deleter of the `struct archive`
     */
    struct ArchiveDeleter {
        auto operator()(struct archive *archive) -> void {
            if (archive != nullptr) {
                archive_write_free(archive);
            }
        }
    };

    /*!
     * \brief Deleter of the `struct archive_entry`
     */
    struct EntryDeleter {
        auto operator()(struct archive_entry *entry) -> void {
            if (entry != nullptr) {
                archive_entry_free(entry);
            }
        }
    };

    /*!
     * \brief Entry information
     */
    struct Entry {
        std::unique_ptr<struct archive_entry, EntryDeleter> header;
        size_t totalSize;
        size_t remainingSize;
    };

    /*!
     * \brief Constructor
     * 
     * \param bufferSize The size of the internal buffer
     */
    Writer(size_t bufferSize = 512) : buffer(bufferSize) {}

    /*!
     * \brief Opens the new output archive
     *
     * \return Nothing on success, else error code
     */
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

    /*!
     * \brief Initializes output archive to use LZ4 compression
     *
     * \return Nothing on success, else error code
     */
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

    /*!
     * \brief Initializes output archive to use ZIP compression
     *
     * \return Nothing on success, else error code
     */
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

    Entry entry;
    std::unique_ptr<struct archive, ArchiveDeleter> archive;
    std::queue<std::string> files;
    std::ifstream input;
    Buffer buffer;
};

} // namespace compression
