#include "compression.h"
#include <archive_entry.h>
#include <chrono>
#include <iostream>

#define USE_ZIP 1
#define NONBLOCK 1

int custom_open(struct archive *archive, void *data) {
    std::cout << "> Custom Open" << std::endl;
    return ARCHIVE_OK;
}

la_ssize_t custom_write(struct archive *archive, void *data, const void *buffer, size_t length) {
    std::string s(reinterpret_cast<const char *>(buffer), length);
    std::cerr << s << std::flush;
    return length;
}

int custom_close(struct archive *archive, void *data) {
    std::cout << std::endl << "> Custom Close" << std::endl;
    return ARCHIVE_OK;
}

int custom_free(struct archive *archive, void *data) {
    std::cout << "> Custom Free" << std::endl;
    return ARCHIVE_OK;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "Missing argument" << std::endl;
        return 1;
    }

    int offset = 0;
    compression::Writer::Pointer writer;

    if (std::string(argv[1]) == "file") {
        auto res = compression::Writer::open(argv[2], compression::ArchiveType::TarLz4);
        if (!res) {
            std::cerr << "Failed to open output file " << static_cast<int>(res.error()) << std::endl;
            return 1;
        }
        writer.swap(res.value());
        offset = 1;
    } else if (std::string(argv[1]) == "cerr") {
        auto res = compression::Writer::open(compression::ArchiveType::TarLz4, &custom_open, &custom_write,
                                             &custom_close, &custom_free);
        if (!res) {
            std::cerr << "Failed to open output file" << std::endl;
            return 1;
        }
        writer.swap(res.value());
    } else {
        std::cerr << "Unknown output" << std::endl;
        return 1;
    }

    while (true) {
        std::string input;
        std::cerr << "Enter filename: " << std::flush;
        std::cin >> input;

        if (input == "exit") {
            break;
        }

        if (!writer->add_file(input)) {
            std::cerr << "Invalid file input" << std::endl;
            continue;
        }

#if NONBLOCK
        auto now = std::chrono::system_clock::now();
        compression::Writer::Result<compression::State> res;
        do {
            res = writer->write(compression::Mode::NonBlock);
            if (!res) {
                std::cerr << "Failed to write zip file " << static_cast<int>(res.error()) << std::endl;
                return 1;
            }
        } while (res.has_value() && res.value() == compression::State::InProgress);
        auto diff =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - now).count();
        std::cerr << "Took " << diff << "ms" << std::endl;
#else
        auto res = writer.write(compression::Mode::Block);
        if (!res) {
            std::cerr << "Failed to write zip file " << static_cast<int>(res.error()) << std::endl;
            return 1;
        }
#endif
    }

    writer->close();

    return 0;
}
