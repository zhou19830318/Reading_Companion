#pragma once
#include <HalStorage.h>
#include <Logging.h>

#include <iostream>

namespace serialization {
// Upper bound for any serialized string (chapter titles, hrefs, paths, keys).
// A torn or corrupt length field is rejected here instead of reaching
// std::string::resize and throwing length_error, which terminates the process
// on this -fno-exceptions target. 8 KB dwarfs every legitimate value in the
// formats we read and keeps a worst-case resize well inside the ~47 KB
// MaxAlloc headroom.
constexpr uint32_t MAX_SERIALIZED_STRING_LEN = 8192;

template <typename T>
void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void writePod(HalFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
void readPod(std::istream& is, T& value) {
  value = T{};  // zero-init so a failed/partial read leaves 0, not stack garbage
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
void readPod(HalFile& file, T& value) {
  value = T{};  // zero-init so a failed/partial read leaves 0, not stack garbage
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

inline void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

inline void writeString(HalFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

inline bool readString(std::istream& is, std::string& s) {
  uint32_t len;
  readPod(is, len);
  if (len > MAX_SERIALIZED_STRING_LEN) {
    LOG_ERR("SER", "serialized string length %u exceeds cap", len);
    s.clear();
    return false;
  }
  s.resize(len);
  if (len) is.read(&s[0], len);
  return static_cast<bool>(is);
}

inline bool readString(HalFile& file, std::string& s) {
  uint32_t len;
  readPod(file, len);
  if (len > MAX_SERIALIZED_STRING_LEN) {
    LOG_ERR("SER", "serialized string length %u exceeds cap at pos %u", len, static_cast<unsigned>(file.position()));
    s.clear();
    return false;
  }
  s.resize(len);
  return len == 0 || file.read(&s[0], len) == static_cast<int>(len);
}
}  // namespace serialization
