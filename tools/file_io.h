// Copyright (c) the JPEG XL Project Authors.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef TOOLS_FILE_IO_H_
#define TOOLS_FILE_IO_H_

#include <sys/stat.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <string>
#include <utility>
#include <vector>

#include "lib/base/compiler_specific.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace jpegxl {
namespace tools {

// RAII, ensures files are closed even when returning early.
class FileWrapper {
 public:
  FileWrapper(const FileWrapper& other) = delete;
  FileWrapper& operator=(const FileWrapper& other) = delete;

  explicit FileWrapper(const std::string& pathname, const char* mode)
      : file_(pathname == "-" ? (mode[0] == 'r' ? stdin : stdout)
                              : fopen(pathname.c_str(), mode)),
        close_on_delete_(pathname != "-") {
#ifdef _WIN32
    struct __stat64 s = {};
    int err = _stat64(pathname.c_str(), &s);
    const bool is_file = (s.st_mode & S_IFREG) != 0;
    if (pathname == "-") {
      err |= _setmode(_fileno(file_), _O_BINARY);
    }
#else
    struct stat s = {};
    int err = stat(pathname.c_str(), &s);
    const bool is_file = S_ISREG(s.st_mode);
#endif
    if (err == 0 && is_file) {
      size_ = s.st_size;
    }
  }

  ~FileWrapper() {
    if (file_ != nullptr && close_on_delete_) {
      const int err = fclose(file_);
      if (err) {
        fprintf(stderr,
                "Could not close file\n"
                "Error: %s",
                strerror(errno));
      }
    }
  }

  // We intend to use FileWrapper as a replacement of FILE.
  // NOLINTNEXTLINE(google-explicit-constructor)
  operator FILE*() const { return file_; }

  int64_t size() const { return size_; }

 private:
  FILE* const file_;
  bool close_on_delete_ = true;
  int64_t size_ = -1;
};

template <typename ContainerType>
static inline bool ReadFile(FileWrapper& f, ContainerType* JXL_RESTRICT bytes) {
  if (!f) return false;

  // Get size of file in bytes
  const int64_t size = f.size();
  if (size < 0) {
    // Size is unknown, loop reading chunks until EOF.
    bytes->clear();
    std::list<std::vector<uint8_t>> chunks;

    size_t total_size = 0;
    while (true) {
      std::vector<uint8_t> chunk(16 * 1024);
      const size_t bytes_read = fread(chunk.data(), 1, chunk.size(), f);
      if (ferror(f) || bytes_read > chunk.size()) {
        return false;
      }

      chunk.resize(bytes_read);
      total_size += bytes_read;
      if (bytes_read != 0) {
        chunks.emplace_back(std::move(chunk));
      }
      if (feof(f)) {
        break;
      }
    }
    bytes->resize(total_size);
    size_t pos = 0;
    for (const auto& chunk : chunks) {
      memcpy(bytes->data() + pos, chunk.data(), chunk.size());
      pos += chunk.size();
    }
  } else {
    // Size is known, read the file directly.
    bytes->resize(static_cast<size_t>(size));

    const size_t bytes_read = fread(bytes->data(), 1, bytes->size(), f);
    if (bytes_read != static_cast<size_t>(size)) return false;
  }

  return true;
}

namespace pipes {

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static inline uint64_t ExtractHandle(const std::string& pathname) {
  int extIndex = pathname.find('.', 1);
  if (extIndex <= 0) {
    fprintf(stderr, "File extension not found\n");
    return 0;
  }
  std::string handleStr = pathname.substr(1, extIndex - 1);
  fprintf(stderr, "%s",  handleStr.c_str());
  return atoi(handleStr.c_str());
}

static inline bool ReadFromPipe(const std::string& pathname, void** bytes, int* size) {
  fprintf(stderr, "Reading from pipe ");
  uint64_t handle = ExtractHandle(pathname);
  fprintf(stderr, "\n");
  if (handle == 0) return false;
  unsigned long read;
  if (!ReadFile(reinterpret_cast<HANDLE>(handle), size, 4, &read, nullptr))
    return false;
  if (read < 4) return false;
  if (*size == 0) return false;

  *bytes = new char[*size];
  if (!ReadFile(reinterpret_cast<HANDLE>(handle), *bytes, *size, &read, nullptr))
    return false;

  fprintf(stderr, "Done reading from pipe\n");
  return true;
}

static inline bool WriteToPipe(void const* bytes, int size, const std::string& pathname) {
  fprintf(stderr, "Writing to pipe ");
  uint64_t handle = ExtractHandle(pathname);
  fprintf(stderr, "\n");
  if (handle == 0) return false;
  unsigned long written;
  if (!WriteFile(reinterpret_cast<HANDLE>(handle), &size, 4, &written, nullptr))
    return false;
  if (!WriteFile(reinterpret_cast<HANDLE>(handle), bytes, size, &written, nullptr))
    return false;

  fprintf(stderr, "Done writing to pipe\n");
  return true;
}

}  // namespace pipes

template <typename ContainerType>
static inline bool ReadFile(const std::string& filename,
                            ContainerType* JXL_RESTRICT bytes) {

  if (filename[0] == ':') {
    void* data;
    int size = 0;
    bool result = pipes::ReadFromPipe(filename, &data, &size);
    if (result && size > 0) {
      bytes->resize(static_cast<size_t>(size));
      memcpy(reinterpret_cast<char*>(&(*bytes)[0]), data, size);
      delete data;
    }
    return result;
  }
  fprintf(stderr, "Reading from file %s\n", filename.c_str());

  FileWrapper f(filename, "rb");
  return ReadFile(f, bytes);
}

template <typename ContainerType>
static inline bool WriteFile(const std::string& filename,
                             const ContainerType& bytes) {

  if (filename[0] == ':') {
    return pipes::WriteToPipe(bytes.data(), bytes.size(), filename);
  }
  fprintf(stderr, "Writing to file %s\n", filename.c_str());

  FileWrapper file(filename, "wb");
  if (!file) {
    fprintf(stderr,
            "Could not open %s for writing\n"
            "Error: %s",
            filename.c_str(), strerror(errno));
    return false;
  }
  if (fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
    fprintf(stderr,
            "Could not write to file\n"
            "Error: %s",
            strerror(errno));
    return false;
  }

  return true;
}

}  // namespace tools
}  // namespace jpegxl

#endif  // TOOLS_FILE_IO_H_
