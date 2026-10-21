// EpoCanva P2.4-recovery -- FIPS 180-4 SHA-256 over a file. See pilot_sha256.h for why the
// harness needs it and for what its own correctness is checked against.
#include "pilot_sha256.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace epocanva {
namespace recovery {
namespace {

constexpr std::size_t kBlockSize = 64;
constexpr std::size_t kReadChunk = 16384;

std::uint32_t Rotr(std::uint32_t x, unsigned n) {
  return (x >> n) | (x << (32u - n));
}

std::uint32_t Ch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
  return (x & y) ^ (~x & z);
}

std::uint32_t Maj(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
  return (x & y) ^ (x & z) ^ (y & z);
}

std::uint32_t BSigma0(std::uint32_t x) { return Rotr(x, 2) ^ Rotr(x, 13) ^ Rotr(x, 22); }
std::uint32_t BSigma1(std::uint32_t x) { return Rotr(x, 6) ^ Rotr(x, 11) ^ Rotr(x, 25); }
std::uint32_t SSigma0(std::uint32_t x) { return Rotr(x, 7) ^ Rotr(x, 18) ^ (x >> 3); }
std::uint32_t SSigma1(std::uint32_t x) { return Rotr(x, 17) ^ Rotr(x, 19) ^ (x >> 10); }

// The round constants and the initial hash value are DERIVED here from the definitions in
// FIPS 180-4 4.2.2 and 5.3.3 -- the first 32 bits of the fractional parts of the cube roots
// and square roots of the first 64 and first 8 primes -- rather than typed out as literal
// tables. A transcribed table is exactly the kind of bug that makes a hash agree with
// nothing and stay undetected, so this file takes the two lines of definition over 128
// numerals. The probe that checks this implementation against the published sample digests
// is what proves the derivation, not the other way round.
struct Constants {
  std::uint32_t k[64];
  std::uint32_t h0[8];
};

bool IsPrime(int n) {
  if (n < 2) {
    return false;
  }
  for (int d = 2; d * d <= n; ++d) {
    if (n % d == 0) {
      return false;
    }
  }
  return true;
}

// First 32 bits of the fractional part of x. long double keeps 64 bits of mantissa, which
// leaves 32 clear bits with room to spare for the two roots below.
std::uint32_t FractionTop32(long double x) {
  const long double frac = x - std::floor(x);
  return static_cast<std::uint32_t>(std::floor(frac * 4294967296.0L));
}

Constants BuildConstants() {
  Constants out;
  int found = 0;
  for (int n = 2; found < 64; ++n) {
    if (!IsPrime(n)) {
      continue;
    }
    out.k[found] = FractionTop32(std::cbrt(static_cast<long double>(n)));
    if (found < 8) {
      out.h0[found] = FractionTop32(std::sqrt(static_cast<long double>(n)));
    }
    ++found;
  }
  return out;
}

const Constants& ConstantsOnce() {
  static const Constants kBuilt = BuildConstants();
  return kBuilt;
}


void Transform(std::array<std::uint32_t, 8>* h, const std::uint8_t* block) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24) |
           (static_cast<std::uint32_t>(block[4 * i + 1]) << 16) |
           (static_cast<std::uint32_t>(block[4 * i + 2]) << 8) |
           static_cast<std::uint32_t>(block[4 * i + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    w[i] = SSigma1(w[i - 2]) + w[i - 7] + SSigma0(w[i - 15]) + w[i - 16];
  }
  std::uint32_t a = (*h)[0], b = (*h)[1], c = (*h)[2], d = (*h)[3];
  std::uint32_t e = (*h)[4], f = (*h)[5], g = (*h)[6], hh = (*h)[7];
  for (int i = 0; i < 64; ++i) {
        const std::uint32_t t1 = hh + BSigma1(e) + Ch(e, f, g) + ConstantsOnce().k[i] +
                   w[i];
    const std::uint32_t t2 = BSigma0(a) + Maj(a, b, c);
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  (*h)[0] += a;
  (*h)[1] += b;
  (*h)[2] += c;
  (*h)[3] += d;
  (*h)[4] += e;
  (*h)[5] += f;
  (*h)[6] += g;
  (*h)[7] += hh;
}

std::array<std::uint32_t, 8> InitialHash() {
  std::array<std::uint32_t, 8> out;
  for (int i = 0; i < 8; ++i) {
    out[static_cast<std::size_t>(i)] = ConstantsOnce().h0[i];
  }
  return out;
}

// One-shot accumulator: bytes go in, the digest comes out, and only one block is buffered.
class Sha256 {
 public:
  void Update(const std::uint8_t* data, std::size_t len) {
    total_ += len;
    while (len > 0) {
      const std::size_t take =
          (kBlockSize - fill_ < len) ? kBlockSize - fill_ : len;
      std::memcpy(buf_ + fill_, data, take);
      fill_ += take;
      data += take;
      len -= take;
      if (fill_ == kBlockSize) {
        Transform(&h_, buf_);
        fill_ = 0;
      }
    }
  }

  std::string Finish() {
    const std::size_t pad_len =
        (fill_ < 56) ? (56 - fill_) : (56 + kBlockSize - fill_);
    std::vector<std::uint8_t> tail;
    tail.push_back(0x80);
    for (std::size_t i = 1; i < pad_len; ++i) {
      tail.push_back(0x00);
    }
    const unsigned long long bits = total_ * 8ull;
    for (int i = 7; i >= 0; --i) {
      tail.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFFu));
    }
    Update(tail.data(), tail.size());
    char hex[65];
    for (int i = 0; i < 8; ++i) {
      std::snprintf(hex + 8 * i, 9, "%08x", h_[static_cast<std::size_t>(i)]);
    }
    return std::string(hex, 64);
  }

 private:
  std::array<std::uint32_t, 8> h_ = InitialHash();
  std::uint8_t buf_[kBlockSize] = {0};
  std::size_t fill_ = 0;
  unsigned long long total_ = 0;
};

}  // namespace

std::string Sha256HexOfFile(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return std::string();
  }
  Sha256 sha;
  std::vector<char> chunk(kReadChunk);
  for (;;) {
    in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const std::streamsize got = in.gcount();
    if (got <= 0) {
      break;
    }
    sha.Update(reinterpret_cast<const std::uint8_t*>(chunk.data()),
               static_cast<std::size_t>(got));
    if (static_cast<std::size_t>(got) < chunk.size()) {
      break;  // a short read is the end of the file
    }
  }
  in.clear();
  if (in.bad()) {
    return std::string();
  }
  return sha.Finish();
}

}  // namespace recovery
}  // namespace epocanva
