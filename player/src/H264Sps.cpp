#include "H264Sps.h"

#include <algorithm>
#include <cstring>

namespace sl {
namespace {

constexpr int kHighProfiles[] = {100, 110, 122, 244, 44, 83, 86,
                                 118, 128, 138, 139, 134, 135};

bool isHighProfile(int profile) {
    for (int value : kHighProfiles) {
        if (value == profile)
            return true;
    }
    return false;
}

class BitReader {
public:
    explicit BitReader(const std::vector<uint8_t>& data)
        : data_(data), bitCount_(data.size() * 8) {}

    bool bit(int& value) {
        if (pos_ >= bitCount_)
            return false;
        value = (data_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1;
        ++pos_;
        return true;
    }

    bool bits(int count, uint32_t& value) {
        value = 0;
        for (int i = 0; i < count; i++) {
            int bitValue = 0;
            if (!bit(bitValue))
                return false;
            value = (value << 1) | (uint32_t)bitValue;
        }
        return true;
    }

    bool ue(uint32_t& value) {
        int zeros = 0;
        while (true) {
            int bitValue = 0;
            if (!bit(bitValue))
                return false;
            if (bitValue)
                break;
            if (++zeros > 31)
                return false;
        }
        uint32_t rest = 0;
        if (zeros > 0 && !bits(zeros, rest))
            return false;
        value = (1u << zeros) - 1u + rest;
        return true;
    }

    bool se(int32_t& value) {
        uint32_t code = 0;
        if (!ue(code))
            return false;
        value = (code & 1) ? (int32_t)((code + 1) / 2) : -(int32_t)(code / 2);
        return true;
    }

    size_t pos() const { return pos_; }

private:
    const std::vector<uint8_t>& data_;
    size_t bitCount_;
    size_t pos_ = 0;
};

std::vector<uint8_t> removeEmulation(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    out.reserve(size);
    int zeros = 0;
    for (size_t i = 0; i < size; i++) {
        uint8_t byte = data[i];
        if (zeros >= 2 && byte == 0x03) {
            zeros = 0;
            continue;
        }
        out.push_back(byte);
        zeros = (byte == 0) ? zeros + 1 : 0;
    }
    return out;
}

std::vector<uint8_t> addEmulation(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    out.reserve(data.size() + 8);
    int zeros = 0;
    for (uint8_t byte : data) {
        if (zeros >= 2 && byte <= 0x03) {
            out.push_back(0x03);
            zeros = 0;
        }
        out.push_back(byte);
        zeros = (byte == 0) ? zeros + 1 : 0;
    }
    return out;
}

std::vector<uint8_t> bytesToBits(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> bits;
    bits.reserve(data.size() * 8);
    for (uint8_t byte : data) {
        for (int i = 7; i >= 0; i--)
            bits.push_back((byte >> i) & 1);
    }
    return bits;
}

std::vector<uint8_t> bitsToBytes(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> out((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); i++) {
        if (bits[i])
            out[i >> 3] |= (uint8_t)(1u << (7 - (i & 7)));
    }
    return out;
}

std::vector<uint8_t> ueBits(uint32_t value) {
    uint64_t code = (uint64_t)value + 1;
    int length = 0;
    for (uint64_t temp = code; temp; temp >>= 1)
        length++;
    std::vector<uint8_t> bits;
    bits.reserve((size_t)length * 2);
    bits.insert(bits.end(), (size_t)length - 1, 0);
    for (int i = length - 1; i >= 0; i--)
        bits.push_back((uint8_t)((code >> i) & 1));
    return bits;
}

bool skipScalingList(BitReader& reader, int size) {
    int lastScale = 8;
    int nextScale = 8;
    for (int i = 0; i < size; i++) {
        if (nextScale != 0) {
            int32_t delta = 0;
            if (!reader.se(delta))
                return false;
            nextScale = (lastScale + delta + 256) % 256;
        }
        lastScale = (nextScale == 0) ? lastScale : nextScale;
    }
    return true;
}

bool skipHrd(BitReader& reader) {
    uint32_t value = 0;
    uint32_t cpbCount = 0;
    if (!reader.ue(cpbCount))
        return false;
    if (!reader.bits(4, value) || !reader.bits(4, value))
        return false;
    for (uint32_t i = 0; i <= cpbCount; i++) {
        if (!reader.ue(value) || !reader.ue(value))
            return false;
        int bit = 0;
        if (!reader.bit(bit))
            return false;
    }
    return reader.bits(5, value) && reader.bits(5, value) &&
           reader.bits(5, value) && reader.bits(5, value);
}

struct ParsedSps {
    H264SpsInfo info;
    size_t refsStart = 0;
    size_t refsEnd = 0;
    size_t reorderStart = 0;
    size_t reorderEnd = 0;
    size_t bufferingStart = 0;
    size_t bufferingEnd = 0;
};

bool parsePayload(const std::vector<uint8_t>& rbsp, ParsedSps& out) {
    if (rbsp.size() < 4)
        return false;

    BitReader reader(rbsp);
    uint32_t value = 0;
    if (!reader.bits(8, value))
        return false;
    int profile = (int)value;
    if (!reader.bits(8, value) || !reader.bits(8, value))
        return false;

    uint32_t spsId = 0;
    if (!reader.ue(spsId))
        return false;

    if (isHighProfile(profile)) {
        uint32_t chromaFormat = 0;
        if (!reader.ue(chromaFormat))
            return false;
        if (chromaFormat == 3) {
            int separate = 0;
            if (!reader.bit(separate))
                return false;
        }
        uint32_t temp = 0;
        if (!reader.ue(temp) || !reader.ue(temp))
            return false;
        int flag = 0;
        if (!reader.bit(flag))
            return false;
        if (!reader.bit(flag))
            return false;
        if (flag) {
            int count = (chromaFormat != 3) ? 8 : 12;
            for (int i = 0; i < count; i++) {
                int present = 0;
                if (!reader.bit(present))
                    return false;
                if (present && !skipScalingList(reader, i < 6 ? 16 : 64))
                    return false;
            }
        }
    }

    uint32_t temp = 0;
    if (!reader.ue(temp)) // log2_max_frame_num_minus4
        return false;

    uint32_t pocType = 0;
    if (!reader.ue(pocType))
        return false;
    if (pocType == 0) {
        if (!reader.ue(temp))
            return false;
    } else if (pocType == 1) {
        int flag = 0;
        int32_t offset = 0;
        if (!reader.bit(flag) || !reader.se(offset) || !reader.se(offset))
            return false;
        uint32_t cycle = 0;
        if (!reader.ue(cycle))
            return false;
        for (uint32_t i = 0; i < cycle; i++) {
            if (!reader.se(offset))
                return false;
        }
    }

    out.refsStart = reader.pos();
    if (!reader.ue(temp))
        return false;
    out.refsEnd = reader.pos();
    out.info.refs = (int)temp;

    int flag = 0;
    if (!reader.bit(flag)) // gaps
        return false;
    if (!reader.ue(temp) || !reader.ue(temp)) // width/height
        return false;
    int frameMbsOnly = 0;
    if (!reader.bit(frameMbsOnly))
        return false;
    if (!frameMbsOnly) {
        if (!reader.bit(flag))
            return false;
    }
    if (!reader.bit(flag)) // direct_8x8
        return false;
    if (!reader.bit(flag)) // cropping
        return false;
    if (flag) {
        for (int i = 0; i < 4; i++) {
            if (!reader.ue(temp))
                return false;
        }
    }

    int vuiPresent = 0;
    if (!reader.bit(vuiPresent))
        return false;
    if (!vuiPresent)
        return false; // 无 VUI，无法确认 reorder/buffering

    int aspectPresent = 0;
    if (!reader.bit(aspectPresent))
        return false;
    if (aspectPresent) {
        if (!reader.bits(8, value))
            return false;
        if (value == 255) {
            uint32_t dummy = 0;
            if (!reader.bits(16, dummy) || !reader.bits(16, dummy))
                return false;
        }
    }

    int overscanPresent = 0;
    if (!reader.bit(overscanPresent))
        return false;
    if (overscanPresent) {
        if (!reader.bit(flag))
            return false;
    }

    int videoSignalPresent = 0;
    if (!reader.bit(videoSignalPresent))
        return false;
    if (videoSignalPresent) {
        if (!reader.bits(3, value) || !reader.bit(flag))
            return false;
        int colourPresent = 0;
        if (!reader.bit(colourPresent))
            return false;
        if (colourPresent) {
            uint32_t dummy = 0;
            if (!reader.bits(8, dummy) || !reader.bits(8, dummy) ||
                !reader.bits(8, dummy))
                return false;
        }
    }

    int chromaLocPresent = 0;
    if (!reader.bit(chromaLocPresent))
        return false;
    if (chromaLocPresent) {
        if (!reader.ue(temp) || !reader.ue(temp))
            return false;
    }

    int timingPresent = 0;
    if (!reader.bit(timingPresent))
        return false;
    if (timingPresent) {
        uint32_t dummy = 0;
        if (!reader.bits(32, dummy) || !reader.bits(32, dummy) ||
            !reader.bit(flag))
            return false;
    }

    int nalHrd = 0;
    if (!reader.bit(nalHrd))
        return false;
    if (nalHrd && !skipHrd(reader))
        return false;
    int vclHrd = 0;
    if (!reader.bit(vclHrd))
        return false;
    if (vclHrd && !skipHrd(reader))
        return false;
    if (nalHrd || vclHrd) {
        if (!reader.bit(flag))
            return false;
    }
    if (!reader.bit(flag)) // pic_struct
        return false;
    if (!reader.bit(flag)) // bitstream_restriction
        return false;

    if (!reader.bit(flag)) // motion_vectors_over_pic_boundaries
        return false;
    if (!reader.ue(temp) || !reader.ue(temp)) // max_bytes/max_bits
        return false;
    if (!reader.ue(temp) || !reader.ue(temp)) // mv lengths
        return false;

    out.reorderStart = reader.pos();
    if (!reader.ue(temp))
        return false;
    out.reorderEnd = reader.pos();
    out.info.reorder = (int)temp;

    out.bufferingStart = reader.pos();
    if (!reader.ue(temp))
        return false;
    out.bufferingEnd = reader.pos();
    out.info.buffering = (int)temp;

    out.info.valid = true;
    return true;
}

size_t findNextStartCode(const uint8_t* data, size_t size, size_t from) {
    for (size_t i = from; i + 2 < size; i++) {
        if (data[i] != 0 || data[i + 1] != 0)
            continue;
        if (data[i + 2] == 1)
            return i;
        if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1)
            return i;
    }
    return size;
}

} // namespace

H264SpsInfo parseSpsNal(const uint8_t* nal, size_t size) {
    H264SpsInfo result;
    if (!nal || size < 5 || (nal[0] & 0x1f) != 7)
        return result;
    std::vector<uint8_t> rbsp = removeEmulation(nal + 1, size - 1);
    ParsedSps parsed;
    if (!parsePayload(rbsp, parsed))
        return result;
    result = parsed.info;
    return result;
}

H264SpsInfo parseAvcC(const uint8_t* extradata, size_t size) {
    H264SpsInfo result;
    if (!extradata || size < 8)
        return result;

    size_t pos = 5;
    int spsCount = extradata[pos++] & 0x1f;
    for (int i = 0; i < spsCount; i++) {
        if (pos + 2 > size)
            return result;
        int length = (extradata[pos] << 8) | extradata[pos + 1];
        pos += 2;
        if (length <= 0 || pos + (size_t)length > size)
            return result;
        H264SpsInfo info = parseSpsNal(extradata + pos, (size_t)length);
        pos += (size_t)length;
        if (info.valid) {
            result = info;
            break;
        }
    }
    return result;
}

bool rewriteSpsNal(std::vector<uint8_t>& nal, int refs, int reorder, int buffering) {
    if (nal.size() < 6 || (nal[0] & 0x1f) != 7)
        return false;

    std::vector<uint8_t> rbsp = removeEmulation(nal.data() + 1, nal.size() - 1);
    ParsedSps parsed;
    if (!parsePayload(rbsp, parsed))
        return false;

    if (parsed.info.refs <= refs && parsed.info.reorder <= reorder &&
        parsed.info.buffering <= buffering) {
        return true;
    }
    if (parsed.info.reorder < 0 || parsed.info.buffering < 0)
        return false;

    int newRefs = std::min(parsed.info.refs, refs);
    int newReorder = std::min(parsed.info.reorder, reorder);
    int newBuffering = std::min(parsed.info.buffering, buffering);

    std::vector<uint8_t> bits = bytesToBits(rbsp);
    std::vector<uint8_t> updated;
    updated.reserve(bits.size() + 16);
    updated.insert(updated.end(), bits.begin(), bits.begin() + parsed.refsStart);
    std::vector<uint8_t> encoded = ueBits((uint32_t)newRefs);
    updated.insert(updated.end(), encoded.begin(), encoded.end());
    updated.insert(updated.end(), bits.begin() + parsed.refsEnd,
                   bits.begin() + parsed.reorderStart);
    encoded = ueBits((uint32_t)newReorder);
    updated.insert(updated.end(), encoded.begin(), encoded.end());
    updated.insert(updated.end(), bits.begin() + parsed.reorderEnd,
                   bits.begin() + parsed.bufferingStart);
    encoded = ueBits((uint32_t)newBuffering);
    updated.insert(updated.end(), encoded.begin(), encoded.end());
    updated.insert(updated.end(), bits.begin() + parsed.bufferingEnd, bits.end());

    std::vector<uint8_t> payload = addEmulation(bitsToBytes(updated));
    uint8_t header = nal[0];
    nal.clear();
    nal.push_back(header);
    nal.insert(nal.end(), payload.begin(), payload.end());
    return true;
}

bool patchAvcC(std::vector<uint8_t>& extradata, int refs, int reorder, int buffering,
               bool* changed) {
    if (changed)
        *changed = false;
    if (extradata.size() < 8)
        return false;

    size_t pos = 5;
    int spsCount = extradata[pos++] & 0x1f;
    if (spsCount <= 0)
        return false;

    std::vector<uint8_t> out;
    out.insert(out.end(), extradata.begin(), extradata.begin() + pos);

    for (int i = 0; i < spsCount; i++) {
        if (pos + 2 > extradata.size())
            return false;
        int length = (extradata[pos] << 8) | extradata[pos + 1];
        pos += 2;
        if (length <= 0 || pos + (size_t)length > extradata.size())
            return false;

        std::vector<uint8_t> nal(extradata.begin() + pos,
                                 extradata.begin() + pos + length);
        pos += (size_t)length;

        H264SpsInfo info = parseSpsNal(nal.data(), nal.size());
        if (!info.valid)
            return false;

        if (info.refs > refs || info.reorder > reorder || info.buffering > buffering) {
            std::vector<uint8_t> rewritten = nal;
            if (!rewriteSpsNal(rewritten, refs, reorder, buffering))
                return false;
            if (rewritten != nal && changed)
                *changed = true;
            nal.swap(rewritten);
        }

        if (nal.size() > 0xFFFF)
            return false;
        out.push_back((uint8_t)((nal.size() >> 8) & 0xFF));
        out.push_back((uint8_t)(nal.size() & 0xFF));
        out.insert(out.end(), nal.begin(), nal.end());
    }

    out.insert(out.end(), extradata.begin() + pos, extradata.end());
    extradata.swap(out);
    return true;
}

bool annexBContainsSps(const uint8_t* data, size_t size) {
    if (!data)
        return false;
    size_t i = 0;
    while (i + 3 < size) {
        size_t startCodeLength = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
            startCodeLength = 4;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            startCodeLength = 3;
        if (startCodeLength == 0) {
            i++;
            continue;
        }
        size_t nalStart = i + startCodeLength;
        if (nalStart < size && (data[nalStart] & 0x1f) == 7)
            return true;
        i = nalStart + 1;
    }
    return false;
}

H264SpsInfo parseFirstSpsAnnexB(const uint8_t* data, size_t size) {
    H264SpsInfo result;
    if (!data)
        return result;
    size_t i = 0;
    while (i + 3 < size) {
        size_t startCodeLength = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
            startCodeLength = 4;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            startCodeLength = 3;
        if (startCodeLength == 0) {
            i++;
            continue;
        }
        size_t nalStart = i + startCodeLength;
        if (nalStart >= size)
            break;
        size_t nalEnd = findNextStartCode(data, size, nalStart + 1);
        if ((data[nalStart] & 0x1f) == 7)
            return parseSpsNal(data + nalStart, nalEnd - nalStart);
        i = nalStart + 1;
    }
    return result;
}

bool patchAnnexBSps(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                    int refs, int reorder, int buffering, int* patched) {
    if (patched)
        *patched = 0;
    if (!data)
        return false;

    out.clear();
    out.reserve(size + 64);

    size_t last = 0;
    size_t i = 0;
    while (i + 3 < size) {
        size_t startCodeLength = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
            startCodeLength = 4;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            startCodeLength = 3;
        if (startCodeLength == 0) {
            i++;
            continue;
        }

        size_t nalStart = i + startCodeLength;
        if (nalStart >= size)
            break;
        size_t nalEnd = findNextStartCode(data, size, nalStart + 1);

        if ((data[nalStart] & 0x1f) == 7) {
            out.insert(out.end(), data + last, data + nalStart);
            std::vector<uint8_t> nal(data + nalStart, data + nalEnd);
            std::vector<uint8_t> rewritten = nal;
            if (rewriteSpsNal(rewritten, refs, reorder, buffering)) {
                out.insert(out.end(), rewritten.begin(), rewritten.end());
                if (patched)
                    (*patched)++;
            } else {
                out.insert(out.end(), nal.begin(), nal.end());
            }
            last = nalEnd;
            i = nalEnd;
            continue;
        }

        i = nalStart + 1;
    }

    out.insert(out.end(), data + last, data + size);
    return true;
}

} // namespace sl
