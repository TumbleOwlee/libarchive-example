#include <archive.h>
#include <archive_entry.h>
#include <fstream>
#include <iostream>

int custom_open(struct archive *archive, void *data) {
  std::cout << "> Custom Open" << std::endl;
  return ARCHIVE_OK;
}

la_ssize_t custom_write(struct archive *archive, void *data, const void *buffer,
                        size_t length) {
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
  if (argc < 2) {
    std::cerr << "Missing argument" << std::endl;
    return 1;
  }
  auto *archive = archive_write_new();
  if (archive == nullptr) {
    std::cerr << "Unable to create archive reader" << std::endl;
    return 1;
  }

  if (archive_write_set_format_zip(archive) != ARCHIVE_OK) {
    std::cerr << "Unable to set output file to zip" << std::endl;
    return 1;
  }

  if (archive_write_zip_set_compression_deflate(archive) != ARCHIVE_OK) {
    std::cerr << "Unanble to set compresssion" << std::endl;
    return 1;
  }

  if (std::string(argv[1]) == "file") {
    if (archive_write_open_filename(archive, "archive.zip") != ARCHIVE_OK) {
      std::cerr << "Failed to open output file" << std::endl;
      return 1;
    }
  } else if (std::string(argv[1]) == "cerr") {
    if (archive_write_open2(archive, nullptr, &custom_open, &custom_write,
                            &custom_close, &custom_free) != ARCHIVE_OK) {
      std::cerr << "Failed to set custom callbacks" << std::endl;
      return 1;
    }
  } else {
    std::cerr << "Unknown output" << std::endl;
    return 1;
  }

  for (int i = 2; i < argc; ++i) {
    std::ifstream input;
    input.open(argv[i]);
    if (!input.is_open()) {
      std::cerr << "Failed to open input file" << std::endl;
      return 1;
    }

    size_t read = 0;
    char buffer[512];

    auto *header = archive_entry_new();
    archive_entry_set_pathname(header, argv[i]);
    archive_entry_set_filetype(header, AE_IFREG);

    if (archive_write_header(archive, header) != ARCHIVE_OK) {
      std::cerr << "Failed to write header" << std::endl;
      return 1;
    }

    do {
      read = input.readsome(&buffer[0], 512);
      if (archive_write_data(archive, &buffer[0], read) != read) {
        std::cerr << "Failed to write data to archive" << std::endl;
        return 1;
      }
    } while (read > 0);
  }

  if (archive_write_free(archive) != ARCHIVE_OK) {
    std::cerr << "Failed to free archive" << std::endl;
    return 1;
  }

  return 0;
}
