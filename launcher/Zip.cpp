#include "Zip.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace Launcher {

namespace {

constexpr uint32_t kEndSignature = 0x06054b50;
constexpr uint32_t kCentralSignature = 0x02014b50;
constexpr uint32_t kLocalSignature = 0x04034b50;
constexpr size_t kEndSize = 22;
constexpr size_t kMaxComment = 0xFFFF;
constexpr size_t kCentralSize = 46;
constexpr size_t kLocalSize = 30;

uint16_t Read16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t Read32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// DEFLATE, decoded a code at a time by counting lengths, which is slow next to a table-driven decoder but small, and
// a ROM takes a second or two at most. It stops as soon as it has the output it was asked for.
class Inflater {
  public:
    Inflater(const uint8_t* in, size_t inSize, std::vector<uint8_t>& out, size_t want)
        : mIn(in), mInSize(inSize), mOut(out), mWant(want) {
    }

    bool Run() {
        bool last = false;
        while (!last && mOut.size() < mWant) {
            last = Bits(1) != 0;
            int type = Bits(2);
            bool ok = type == 0 ? Stored() : type == 1 ? Fixed() : type == 2 ? Dynamic() : false;
            if (!ok || mFailed) {
                return false;
            }
        }
        return !mFailed;
    }

  private:
    static constexpr int kMaxBits = 15;
    static constexpr int kMaxLengths = 286;
    static constexpr int kMaxDistances = 30;

    struct Huffman {
        short count[kMaxBits + 1];
        short symbol[kMaxLengths + kMaxDistances];
    };

    const uint8_t* mIn;
    size_t mInSize;
    size_t mInPos = 0;
    std::vector<uint8_t>& mOut;
    size_t mWant;
    uint32_t mBitBuffer = 0;
    int mBitCount = 0;
    bool mFailed = false;

    int Bits(int need) {
        uint32_t value = mBitBuffer;
        while (mBitCount < need) {
            if (mInPos >= mInSize) {
                mFailed = true;
                return 0;
            }
            value |= static_cast<uint32_t>(mIn[mInPos++]) << mBitCount;
            mBitCount += 8;
        }
        mBitBuffer = value >> need;
        mBitCount -= need;
        return static_cast<int>(value & ((1u << need) - 1));
    }

    int Decode(const Huffman& h) {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int length = 1; length <= kMaxBits; length++) {
            code |= Bits(1);
            if (mFailed) {
                return -1;
            }
            int count = h.count[length];
            if (code - count < first) {
                return h.symbol[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    static void Build(Huffman& h, const short* lengths, int n) {
        memset(h.count, 0, sizeof(h.count));
        for (int i = 0; i < n; i++) {
            h.count[lengths[i]]++;
        }
        short offsets[kMaxBits + 1];
        offsets[1] = 0;
        for (int i = 1; i < kMaxBits; i++) {
            offsets[i + 1] = offsets[i] + h.count[i];
        }
        for (int i = 0; i < n; i++) {
            if (lengths[i] != 0) {
                h.symbol[offsets[lengths[i]]++] = static_cast<short>(i);
            }
        }
    }

    bool Stored() {
        mBitBuffer = 0;
        mBitCount = 0;
        if (mInPos + 4 > mInSize) {
            return false;
        }
        size_t length = Read16(mIn + mInPos);
        mInPos += 4;
        if (mInPos + length > mInSize) {
            return false;
        }
        length = std::min(length, mWant - mOut.size());
        mOut.insert(mOut.end(), mIn + mInPos, mIn + mInPos + length);
        mInPos += length;
        return true;
    }

    bool Codes(const Huffman& lengths, const Huffman& distances) {
        static const short kLengthBase[29] = { 3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
        static const short kLengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        static const short kDistanceBase[30] = { 1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                                 33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                                 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        static const short kDistanceExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
        while (mOut.size() < mWant) {
            int symbol = Decode(lengths);
            if (symbol < 0) {
                return false;
            }
            if (symbol < 256) {
                mOut.push_back(static_cast<uint8_t>(symbol));
            } else if (symbol == 256) {
                return true;
            } else {
                symbol -= 257;
                if (symbol >= 29) {
                    return false;
                }
                int length = kLengthBase[symbol] + Bits(kLengthExtra[symbol]);
                int distanceSymbol = Decode(distances);
                if (distanceSymbol < 0 || distanceSymbol >= 30) {
                    return false;
                }
                size_t distance = static_cast<size_t>(kDistanceBase[distanceSymbol]) + Bits(kDistanceExtra[distanceSymbol]);
                if (mFailed || distance > mOut.size()) {
                    return false;
                }
                for (int i = 0; i < length && mOut.size() < mWant; i++) {
                    mOut.push_back(mOut[mOut.size() - distance]);
                }
            }
        }
        return true;
    }

    bool Fixed() {
        static Huffman sLengths;
        static Huffman sDistances;
        static bool sBuilt = false;
        if (!sBuilt) {
            short lengths[kMaxLengths + 2];
            int i = 0;
            for (; i < 144; i++) lengths[i] = 8;
            for (; i < 256; i++) lengths[i] = 9;
            for (; i < 280; i++) lengths[i] = 7;
            for (; i < 288; i++) lengths[i] = 8;
            Build(sLengths, lengths, 288);
            short distances[kMaxDistances];
            for (i = 0; i < kMaxDistances; i++) distances[i] = 5;
            Build(sDistances, distances, kMaxDistances);
            sBuilt = true;
        }
        return Codes(sLengths, sDistances);
    }

    bool Dynamic() {
        static const short kOrder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
        int lengthCount = Bits(5) + 257;
        int distanceCount = Bits(5) + 1;
        int codeCount = Bits(4) + 4;
        if (mFailed || lengthCount > kMaxLengths || distanceCount > kMaxDistances) {
            return false;
        }
        short lengths[kMaxLengths + kMaxDistances] = {};
        for (int i = 0; i < codeCount; i++) {
            lengths[kOrder[i]] = static_cast<short>(Bits(3));
        }
        Huffman codes;
        Build(codes, lengths, 19);
        memset(lengths, 0, sizeof(lengths));
        int index = 0;
        while (index < lengthCount + distanceCount) {
            int symbol = Decode(codes);
            if (symbol < 0) {
                return false;
            }
            if (symbol < 16) {
                lengths[index++] = static_cast<short>(symbol);
                continue;
            }
            short previous = 0;
            int repeat;
            if (symbol == 16) {
                if (index == 0) {
                    return false;
                }
                previous = lengths[index - 1];
                repeat = 3 + Bits(2);
            } else if (symbol == 17) {
                repeat = 3 + Bits(3);
            } else {
                repeat = 11 + Bits(7);
            }
            if (index + repeat > lengthCount + distanceCount) {
                return false;
            }
            while (repeat-- > 0) {
                lengths[index++] = previous;
            }
        }
        Huffman lengthCode;
        Huffman distanceCode;
        Build(lengthCode, lengths, lengthCount);
        Build(distanceCode, lengths + lengthCount, distanceCount);
        return Codes(lengthCode, distanceCode);
    }
};

} // namespace

bool ListZip(const std::filesystem::path& zip, std::vector<ZipEntry>& entries) {
    std::ifstream file(zip, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    uint64_t size = static_cast<uint64_t>(file.tellg());
    if (size < kEndSize) {
        return false;
    }
    size_t tail = static_cast<size_t>(std::min<uint64_t>(size, kEndSize + kMaxComment));
    std::vector<uint8_t> end(tail);
    file.seekg(static_cast<std::streamoff>(size - tail));
    file.read(reinterpret_cast<char*>(end.data()), static_cast<std::streamsize>(tail));
    if (file.gcount() != static_cast<std::streamsize>(tail)) {
        return false;
    }
    for (size_t i = tail - kEndSize + 1; i-- > 0;) {
        if (Read32(end.data() + i) != kEndSignature) {
            continue;
        }
        uint16_t count = Read16(end.data() + i + 10);
        uint32_t centralSize = Read32(end.data() + i + 12);
        uint32_t centralOffset = Read32(end.data() + i + 16);
        if (static_cast<uint64_t>(centralOffset) + centralSize > size) {
            return false;
        }
        std::vector<uint8_t> central(centralSize);
        file.seekg(centralOffset);
        file.read(reinterpret_cast<char*>(central.data()), centralSize);
        if (file.gcount() != static_cast<std::streamsize>(centralSize)) {
            return false;
        }
        entries.clear();
        size_t at = 0;
        for (uint16_t n = 0; n < count && at + kCentralSize <= central.size(); n++) {
            const uint8_t* p = central.data() + at;
            if (Read32(p) != kCentralSignature) {
                return false;
            }
            size_t nameLength = Read16(p + 28);
            size_t extraLength = Read16(p + 30);
            size_t commentLength = Read16(p + 32);
            if (at + kCentralSize + nameLength > central.size()) {
                return false;
            }
            ZipEntry entry;
            entry.method = Read16(p + 10);
            entry.packedSize = Read32(p + 20);
            entry.size = Read32(p + 24);
            entry.offset = Read32(p + 42);
            entry.name.assign(reinterpret_cast<const char*>(p + kCentralSize), nameLength);
            entries.push_back(std::move(entry));
            at += kCentralSize + nameLength + extraLength + commentLength;
        }
        return true;
    }
    return false;
}

bool ReadZipEntry(const std::filesystem::path& zip, const ZipEntry& entry, uint64_t count, std::vector<uint8_t>& out) {
    std::ifstream file(zip, std::ios::binary);
    uint8_t local[kLocalSize];
    file.seekg(static_cast<std::streamoff>(entry.offset));
    file.read(reinterpret_cast<char*>(local), kLocalSize);
    if (file.gcount() != kLocalSize || Read32(local) != kLocalSignature) {
        return false;
    }
    uint64_t dataAt = entry.offset + kLocalSize + Read16(local + 26) + Read16(local + 28);
    uint64_t want = std::min(count, entry.size);
    // A little deflated data goes a long way, so a peek at the header doesn't read the whole file.
    uint64_t read = want >= entry.size ? entry.packedSize : std::min<uint64_t>(entry.packedSize, want * 8 + 65536);
    std::vector<uint8_t> packed(static_cast<size_t>(read));
    file.seekg(static_cast<std::streamoff>(dataAt));
    file.read(reinterpret_cast<char*>(packed.data()), static_cast<std::streamsize>(read));
    if (file.gcount() != static_cast<std::streamsize>(read)) {
        return false;
    }
    out.clear();
    out.reserve(static_cast<size_t>(want));
    if (entry.method == 0) {
        out.assign(packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(std::min<uint64_t>(want, read)));
        return true;
    }
    if (entry.method != 8) {
        return false;
    }
    Inflater inflater(packed.data(), packed.size(), out, static_cast<size_t>(want));
    return inflater.Run() && out.size() == want;
}

} // namespace Launcher
