// EpoCanva W1 -- small file and digest helpers used by the product shell.
//
// The digest is BCrypt (Windows CNG), never a local implementation. Writes are
// temp-file + MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH), so a crash leaves either
// the old file or the complete new file, never a half-written settings or patch file.
#ifndef EPOCANVA_PRODUCT_IO_H_
#define EPOCANVA_PRODUCT_IO_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace epocanva {
namespace product {

bool ReadFileBytes(const std::filesystem::path& path, std::vector<std::uint8_t>* bytes,
                   std::wstring* why);
bool WriteFileAtomically(const std::filesystem::path& path, const void* data,
                         std::size_t size, std::wstring* why);
bool WriteTextAtomically(const std::filesystem::path& path, const std::string& text,
                         std::wstring* why);

bool Sha256(const void* data, std::size_t size, std::string* lowercase_hex,
            std::wstring* why);
bool Sha256File(const std::filesystem::path& path, std::string* lowercase_hex,
                std::uint64_t* size, std::wstring* why);

bool IsLowercaseHexSha256(const std::string& text);
std::wstring Utf8ToWide(const std::string& text);
std::string WideToUtf8(const std::wstring& text);

}  // namespace product
}  // namespace epocanva

#endif  // EPOCANVA_PRODUCT_IO_H_
