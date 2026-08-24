#include "parity_fixture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace melearner::fixtures {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kSha256BlockBytes = 64;
constexpr std::array<std::uint32_t, 64> kSha256Constants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

class Sha256 final {
public:
    void update(std::span<const std::byte> input) {
        for (const auto value : input) {
            buffer_[buffer_size_++] = std::to_integer<std::uint8_t>(value);
            ++total_bytes_;
            if (buffer_size_ == kSha256BlockBytes) {
                transform(buffer_);
                buffer_size_ = 0;
            }
        }
    }

    [[nodiscard]] std::string finish() {
        const std::uint64_t message_bits = total_bytes_ * 8U;
        const std::array<std::byte, 1> marker = {std::byte{0x80}};
        update(marker);

        const std::array<std::byte, 1> zero = {std::byte{0}};
        while (buffer_size_ != 56U) {
            update(zero);
        }

        std::array<std::byte, 8> length_bytes{};
        for (std::size_t index = 0; index < length_bytes.size(); ++index) {
            const auto shift = static_cast<unsigned>((length_bytes.size() - index - 1U) * 8U);
            length_bytes[index] = std::byte((message_bits >> shift) & 0xffU);
        }
        update(length_bytes);

        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const auto word : state_) {
            output << std::setw(8) << word;
        }
        return output.str();
    }

private:
    void transform(const std::array<std::uint8_t, kSha256BlockBytes>& block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto offset = index * 4U;
            words[index] = (static_cast<std::uint32_t>(block[offset]) << 24U)
                | (static_cast<std::uint32_t>(block[offset + 1U]) << 16U)
                | (static_cast<std::uint32_t>(block[offset + 2U]) << 8U)
                | static_cast<std::uint32_t>(block[offset + 3U]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto s0 = std::rotr(words[index - 15U], 7) ^ std::rotr(words[index - 15U], 18)
                ^ (words[index - 15U] >> 3U);
            const auto s1 = std::rotr(words[index - 2U], 17) ^ std::rotr(words[index - 2U], 19)
                ^ (words[index - 2U] >> 10U);
            words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
        }

        auto a = state_[0];
        auto b = state_[1];
        auto c = state_[2];
        auto d = state_[3];
        auto e = state_[4];
        auto f = state_[5];
        auto g = state_[6];
        auto h = state_[7];

        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choose = (e & f) ^ ((~e) & g);
            const auto temp1 = h + sum1 + choose + kSha256Constants[index] + words[index];
            const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = sum0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_ = {
        0x6a09e667U,
        0xbb67ae85U,
        0x3c6ef372U,
        0xa54ff53aU,
        0x510e527fU,
        0x9b05688cU,
        0x1f83d9abU,
        0x5be0cd19U,
    };
    std::array<std::uint8_t, kSha256BlockBytes> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t total_bytes_ = 0;
};

[[nodiscard]] std::vector<std::byte> read_binary_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot read fixture file: " + path.string());
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("cannot measure fixture file: " + path.string());
    }
    const auto size = static_cast<std::size_t>(end);
    std::vector<std::byte> bytes(size);
    input.seekg(0);
    if (size != 0U) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    }
    if (!input) {
        throw std::runtime_error("cannot read complete fixture file: " + path.string());
    }
    return bytes;
}

void write_binary_file(const fs::path& path, std::span<const std::byte> bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot write fixture file: " + path.string());
    }
    if (!bytes.empty()) {
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    if (!output) {
        throw std::runtime_error("cannot finish fixture file: " + path.string());
    }
    output.close();
    if (output.fail()) {
        throw std::runtime_error("cannot close fixture file: " + path.string());
    }
}

void write_text_file(const fs::path& path, std::string_view text) {
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    write_binary_file(path, bytes);
}

[[nodiscard]] std::string path_to_utf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] fs::path path_from_utf8(std::string_view value) {
    std::u8string converted;
    converted.reserve(value.size());
    for (const auto character : value) {
        converted.push_back(static_cast<char8_t>(static_cast<unsigned char>(character)));
    }
    return fs::path(converted);
}

[[nodiscard]] std::string json_escape(std::string_view input) {
    std::string output;
    output.reserve(input.size() + 8U);
    constexpr char digits[] = "0123456789abcdef";
    for (const auto raw : input) {
        const auto value = static_cast<unsigned char>(raw);
        switch (value) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (value < 0x20U) {
                output += "\\u00";
                output.push_back(digits[value >> 4U]);
                output.push_back(digits[value & 0x0fU]);
            } else {
                output.push_back(raw);
            }
            break;
        }
    }
    return output;
}

[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes) {
    std::uint32_t value = 0xffffffffU;
    for (const auto raw : bytes) {
        value ^= std::to_integer<std::uint8_t>(raw);
        for (int bit = 0; bit < 8; ++bit) {
            value = (value >> 1U) ^ (0xedb88320U & (0U - (value & 1U)));
        }
    }
    return ~value;
}

void write_le16(std::ostream& output, std::uint16_t value) {
    const std::array<char, 2> bytes = {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
    };
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_le32(std::ostream& output, std::uint32_t value) {
    const std::array<char, 4> bytes = {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU),
    };
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::uint16_t read_le16(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::string_view field) {
    if (offset > bytes.size() || bytes.size() - offset < 2U) {
        throw std::runtime_error("truncated ZIP " + std::string(field));
    }
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(bytes[offset])
        | (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 8U));
}

[[nodiscard]] std::uint32_t read_le32(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::string_view field) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) {
        throw std::runtime_error("truncated ZIP " + std::string(field));
    }
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset]))
        | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 8U)
        | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U])) << 16U)
        | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3U])) << 24U);
}

struct ZipEntry {
    std::string name;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint32_t compressed_bytes = 0;
    std::uint32_t uncompressed_bytes = 0;
    std::uint32_t local_offset = 0;
};

[[nodiscard]] std::vector<std::byte> deflate_stored_blocks(std::span<const std::byte> bytes) {
    constexpr std::size_t max_block_bytes = std::numeric_limits<std::uint16_t>::max();
    const auto block_count = std::max<std::size_t>(1U, (bytes.size() + max_block_bytes - 1U) / max_block_bytes);
    std::vector<std::byte> output;
    output.reserve(bytes.size() + block_count * 5U);

    std::size_t offset = 0;
    do {
        const auto length = static_cast<std::uint16_t>(
            std::min(max_block_bytes, bytes.size() - offset));
        const bool final = offset + length == bytes.size();
        const auto inverse_length = static_cast<std::uint16_t>(~length);
        output.push_back(final ? std::byte{0x01} : std::byte{0x00});
        output.push_back(std::byte(length & 0xffU));
        output.push_back(std::byte((length >> 8U) & 0xffU));
        output.push_back(std::byte(inverse_length & 0xffU));
        output.push_back(std::byte((inverse_length >> 8U) & 0xffU));
        output.insert(
            output.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
    } while (offset < bytes.size());
    return output;
}

[[nodiscard]] std::vector<std::byte> inflate_stored_blocks(
    std::span<const std::byte> bytes,
    std::size_t expected_bytes) {
    std::vector<std::byte> output;
    output.reserve(expected_bytes);
    std::size_t offset = 0;
    bool final = false;
    while (!final) {
        if (offset >= bytes.size()) {
            throw std::runtime_error("truncated DEFLATE stored-block header");
        }
        const auto header = std::to_integer<std::uint8_t>(bytes[offset++]);
        final = (header & 0x01U) != 0U;
        if ((header & 0xfeU) != 0U) {
            throw std::runtime_error("DEFLATE fixture uses a non-stored block");
        }
        const auto length = read_le16(bytes, offset, "DEFLATE stored length");
        const auto inverse_length = read_le16(bytes, offset + 2U, "DEFLATE stored inverse length");
        offset += 4U;
        if (static_cast<std::uint16_t>(length ^ inverse_length) != 0xffffU) {
            throw std::runtime_error("DEFLATE stored-block lengths disagree");
        }
        if (offset > bytes.size() || bytes.size() - offset < length
            || output.size() > expected_bytes || expected_bytes - output.size() < length) {
            throw std::runtime_error("DEFLATE stored-block payload is out of bounds");
        }
        output.insert(
            output.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
    }
    if (offset != bytes.size() || output.size() != expected_bytes) {
        throw std::runtime_error("DEFLATE stored-block stream size is inconsistent");
    }
    return output;
}

class ZipWriter final {
public:
    explicit ZipWriter(const fs::path& path) {
        fs::create_directories(path.parent_path());
        output_.open(path, std::ios::binary | std::ios::trunc);
        if (!output_) {
            throw std::runtime_error("cannot create ZIP fixture: " + path.string());
        }
    }

    void add(std::string name, std::span<const std::byte> contents) {
        add_encoded(std::move(name), contents, contents, 0U);
    }

    void add_text(std::string name, std::string_view contents) {
        add(std::move(name), std::as_bytes(std::span(contents.data(), contents.size())));
    }

    void add_deflated_text(std::string name, std::string_view contents) {
        const auto source = std::as_bytes(std::span(contents.data(), contents.size()));
        const auto compressed = deflate_stored_blocks(source);
        add_encoded(std::move(name), source, compressed, 8U);
    }

    void finish() {
        if (finished_) {
            return;
        }
        if (entries_.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("ZIP fixture contains too many entries");
        }
        const auto central_offset_value = output_.tellp();
        if (central_offset_value < 0
            || static_cast<std::uint64_t>(central_offset_value)
                > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("ZIP central directory offset is too large");
        }
        const auto central_offset = static_cast<std::uint32_t>(central_offset_value);

        for (const auto& entry : entries_) {
            write_le32(output_, 0x02014b50U);
            write_le16(output_, 20U);
            write_le16(output_, 20U);
            write_le16(output_, 0x0800U);
            write_le16(output_, entry.method);
            write_le16(output_, 0U);
            write_le16(output_, 0x0021U);
            write_le32(output_, entry.crc);
            write_le32(output_, entry.compressed_bytes);
            write_le32(output_, entry.uncompressed_bytes);
            write_le16(output_, static_cast<std::uint16_t>(entry.name.size()));
            write_le16(output_, 0U);
            write_le16(output_, 0U);
            write_le16(output_, 0U);
            write_le16(output_, 0U);
            write_le32(output_, 0U);
            write_le32(output_, entry.local_offset);
            output_.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        }

        const auto central_end_value = output_.tellp();
        if (central_end_value < 0
            || static_cast<std::uint64_t>(central_end_value)
                > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("ZIP central directory is too large");
        }
        const auto central_end = static_cast<std::uint32_t>(central_end_value);
        const auto central_bytes = central_end - central_offset;
        const auto entry_count = static_cast<std::uint16_t>(entries_.size());

        write_le32(output_, 0x06054b50U);
        write_le16(output_, 0U);
        write_le16(output_, 0U);
        write_le16(output_, entry_count);
        write_le16(output_, entry_count);
        write_le32(output_, central_bytes);
        write_le32(output_, central_offset);
        write_le16(output_, 0U);
        output_.flush();
        if (!output_) {
            throw std::runtime_error("cannot finish ZIP fixture");
        }
        output_.close();
        if (output_.fail()) {
            throw std::runtime_error("cannot close ZIP fixture");
        }
        finished_ = true;
    }

private:
    void add_encoded(
        std::string name,
        std::span<const std::byte> contents,
        std::span<const std::byte> encoded,
        std::uint16_t method) {
        if (finished_) {
            throw std::logic_error("cannot append to finished ZIP fixture");
        }
        if (name.empty() || name.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("invalid ZIP fixture entry name");
        }
        if (contents.size() > std::numeric_limits<std::uint32_t>::max()
            || encoded.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("ZIP fixture entry is too large");
        }
        const auto offset = output_.tellp();
        if (offset < 0 || static_cast<std::uint64_t>(offset) > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("ZIP fixture offset is too large");
        }

        ZipEntry entry{
            .name = std::move(name),
            .method = method,
            .crc = crc32(contents),
            .compressed_bytes = static_cast<std::uint32_t>(encoded.size()),
            .uncompressed_bytes = static_cast<std::uint32_t>(contents.size()),
            .local_offset = static_cast<std::uint32_t>(offset),
        };
        write_le32(output_, 0x04034b50U);
        write_le16(output_, 20U);
        write_le16(output_, 0x0800U);
        write_le16(output_, entry.method);
        write_le16(output_, 0U);
        write_le16(output_, 0x0021U);
        write_le32(output_, entry.crc);
        write_le32(output_, entry.compressed_bytes);
        write_le32(output_, entry.uncompressed_bytes);
        write_le16(output_, static_cast<std::uint16_t>(entry.name.size()));
        write_le16(output_, 0U);
        output_.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        if (!encoded.empty()) {
            output_.write(
                reinterpret_cast<const char*>(encoded.data()),
                static_cast<std::streamsize>(encoded.size()));
        }
        entries_.push_back(std::move(entry));
    }

    std::ofstream output_;
    std::vector<ZipEntry> entries_;
    bool finished_ = false;
};

[[nodiscard]] std::string minimal_pdf_500_pages() {
    constexpr int page_count = 500;
    constexpr int object_count = page_count + 2;

    std::ostringstream output;
    output << "%PDF-1.7\n% melearner deterministic MIT fixture\n";
    std::vector<std::uint64_t> offsets(static_cast<std::size_t>(object_count) + 1U, 0U);

    const auto start_object = [&output, &offsets](int object_id) {
        const auto offset = output.tellp();
        if (offset < 0) {
            throw std::runtime_error("cannot measure PDF fixture object");
        }
        offsets.at(static_cast<std::size_t>(object_id)) = static_cast<std::uint64_t>(offset);
        output << object_id << " 0 obj\n";
    };

    start_object(1);
    output << "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    start_object(2);
    output << "<< /Type /Pages /Count " << page_count << " /Kids [";
    for (int page = 0; page < page_count; ++page) {
        output << (page + 3) << " 0 R ";
    }
    output << "] >>\nendobj\n";

    for (int page = 0; page < page_count; ++page) {
        start_object(page + 3);
        output << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 72 72] /Resources << >> >>\n"
               << "endobj\n";
    }

    const auto xref_offset_value = output.tellp();
    if (xref_offset_value < 0) {
        throw std::runtime_error("cannot measure PDF fixture xref");
    }
    const auto xref_offset = static_cast<std::uint64_t>(xref_offset_value);
    output << "xref\n0 " << (object_count + 1) << "\n";
    output << "0000000000 65535 f \n";
    for (int object_id = 1; object_id <= object_count; ++object_id) {
        output << std::setw(10) << std::setfill('0')
               << offsets.at(static_cast<std::size_t>(object_id)) << " 00000 n \n";
    }
    output << "trailer\n<< /Size " << (object_count + 1) << " /Root 1 0 R >>\n";
    output << "startxref\n" << xref_offset << "\n%%EOF\n";
    return output.str();
}

void write_minimal_docx(const fs::path& path) {
    constexpr std::string_view content_types =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
        "  <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
        "  <Default Extension=\"xml\" ContentType=\"application/xml\"/>\n"
        "  <Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>\n"
        "</Types>\n";
    constexpr std::string_view root_relationships =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
        "  <Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>\n"
        "</Relationships>\n";
    constexpr std::string_view document =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">\n"
        "  <w:body>\n"
        "    <w:p><w:pPr><w:pStyle w:val=\"Heading2\"/></w:pPr><w:r><w:t>Fixture chapter</w:t></w:r></w:p>\n"
        "    <w:p><w:r><w:t>Hello </w:t></w:r><w:r><w:t>world.</w:t></w:r></w:p>\n"
        "    <w:p><w:pPr><w:numPr><w:numId w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>First item</w:t></w:r></w:p>\n"
        "    <w:tbl><w:tr><w:tc><w:p><w:r><w:t>Key</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>Value</w:t></w:r></w:p></w:tc></w:tr></w:tbl>\n"
        "    <w:p><w:r><w:drawing><w:t>omitted image text</w:t></w:drawing></w:r></w:p>\n"
        "    <w:altChunk r:id=\"external\"><w:p><w:r><w:t>omitted imported text</w:t></w:r></w:p></w:altChunk>\n"
        "  </w:body>\n"
        "</w:document>\n";
    constexpr std::string_view document_relationships =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
        "  <Relationship Id=\"external\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/aFChunk\" TargetMode=\"External\" Target=\"https://example.invalid/content\"/>\n"
        "</Relationships>\n";

    ZipWriter zip(path);
    zip.add_deflated_text("[Content_Types].xml", content_types);
    zip.add_deflated_text("_rels/.rels", root_relationships);
    zip.add_deflated_text("word/document.xml", document);
    zip.add_deflated_text("word/_rels/document.xml.rels", document_relationships);
    zip.finish();
}

void write_oversized_docx(const fs::path& path) {
    constexpr std::size_t entry_count = 4'097;
    constexpr std::string_view empty;
    ZipWriter zip(path);
    for (std::size_t index = 0; index < entry_count; ++index) {
        std::array<char, 5> digits{};
        const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), index);
        if (result.ec != std::errc{}) {
            throw std::runtime_error("cannot format DOCX entry index");
        }
        std::string name = "word/e/";
        name.append(digits.size() - static_cast<std::size_t>(result.ptr - digits.data()), '0');
        name.append(digits.data(), result.ptr);
        name += ".xml";
        zip.add_text(std::move(name), empty);
    }
    zip.finish();
}

struct ManifestFixture {
    std::string path;
    FileDigest digest;
    std::string role;
    std::string provenance_kind;
    std::optional<std::string> recipe;
};

[[nodiscard]] std::pair<std::string, std::optional<std::string>> fixture_provenance(
    std::string_view path) {
    if (path.starts_with("fixtures/parity/documents/")) {
        return {"deterministic-generated", std::string("cpp-parity-recipe-v1")};
    }
    if (path == "fixtures/parity/media/Systems 日本語/01 H264 AAC.mp4"
        || path == "fixtures/parity/media/02 Multi audio chapters.mkv"
        || path == "fixtures/parity/media/03 HEVC Main 10.mkv") {
        return {"deterministic-generated", std::string("scripts/generate-media-corpus.sh@n8.1.2")};
    }
    return {"repository-authored", std::nullopt};
}

[[nodiscard]] std::string fixture_role(std::string_view path) {
    if (path == "fixtures/parity/database-current.sql") {
        return "frozen-database-oracle";
    }
    if (path == "fixtures/parity/fixture-manifest-v1.json") {
        return "frozen-manifest-v1";
    }
    if (path == "fixtures/parity/oracle-v1.json") {
        return "behavior-oracle-v1";
    }
    if (path == "fixtures/parity/scanner-v1.json") {
        return "scanner-oracle-v1";
    }
    if (path == "fixtures/parity/watchdog-app.mjs") {
        return "responsiveness-oracle";
    }
    if (path.ends_with(".srt") || path.ends_with(".vtt")) {
        return "media-subtitle";
    }
    if (path.ends_with(".mp4") || path.ends_with(".mkv")) {
        return "media-playback";
    }
    if (path.ends_with("chapters.ffmeta")) {
        return "media-chapter-metadata";
    }
    if (path.ends_with("corrupt-media.bin")) {
        return "malformed-media";
    }
    if (path.ends_with("utf8-text.txt")) {
        return "document-valid-utf8";
    }
    if (path.ends_with("representative.md")) {
        return "document-markdown";
    }
    if (path.ends_with("active-remote.html")) {
        return "document-html-active-remote";
    }
    if (path.ends_with("minimal.docx")) {
        return "document-docx-supported";
    }
    if (path.ends_with("unsupported.doc")) {
        return "document-unsupported";
    }
    if (path.ends_with("malformed-utf8.txt")) {
        return "document-malformed-utf8";
    }
    if (path.ends_with("malformed.docx")) {
        return "document-malformed-docx";
    }
    if (path.ends_with("malformed.pdf")) {
        return "document-malformed-pdf";
    }
    if (path.ends_with("oversized-4097-entries.docx")) {
        return "document-oversized-docx";
    }
    if (path.ends_with("blank-500-pages.pdf")) {
        return "document-pdf-500-pages";
    }
    throw std::runtime_error("fixture has no manifest role: " + std::string(path));
}

[[nodiscard]] std::vector<ManifestFixture> collect_manifest_fixtures(const fs::path& repo_root) {
    const auto parity_root = repo_root / "fixtures/parity";
    if (!fs::is_directory(parity_root)) {
        throw std::runtime_error("missing parity fixture root: " + parity_root.string());
    }

    std::vector<ManifestFixture> files;
    for (const auto& entry : fs::recursive_directory_iterator(parity_root)) {
        const auto status = entry.symlink_status();
        if (fs::is_symlink(status)) {
            throw std::runtime_error("parity fixtures cannot contain symlinks: " + entry.path().string());
        }
        if (!fs::is_regular_file(status)) {
            continue;
        }
        const auto relative = fs::relative(entry.path(), repo_root);
        const auto path = path_to_utf8(relative);
        if (path == "fixtures/parity/fixture-manifest-v2.json") {
            continue;
        }
        if (!is_valid_utf8(std::as_bytes(std::span(path.data(), path.size())))) {
            throw std::runtime_error("fixture path is not valid UTF-8");
        }
        if (canonical_logical_path(path) != path) {
            throw std::runtime_error("fixture path is not canonical POSIX: " + path);
        }
        auto [kind, recipe] = fixture_provenance(path);
        files.push_back(ManifestFixture{
            .path = path,
            .digest = digest_file(entry.path()),
            .role = fixture_role(path),
            .provenance_kind = std::move(kind),
            .recipe = std::move(recipe),
        });
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return std::lexicographical_compare(
            left.path.begin(),
            left.path.end(),
            right.path.begin(),
            right.path.end(),
            [](char left_byte, char right_byte) {
                return static_cast<unsigned char>(left_byte)
                    < static_cast<unsigned char>(right_byte);
            });
    });
    return files;
}

struct SourceGroup {
    std::string_view name;
    std::vector<std::string_view> paths;
};

[[nodiscard]] const std::vector<SourceGroup>& manifest_source_groups() {
    static const std::vector<SourceGroup> groups = {
        {
            "generator",
            {
                "cpp-app/tests/fixtures/parity_fixture.hpp",
                "cpp-app/tests/fixtures/parity_fixture.cpp",
                "cpp-app/tests/fixtures/parity_recipe_v1.cpp",
                "cpp-app/tests/fixtures/parity_fixture_main.cpp",
                "cpp-app/tests/fixtures/parity_fixture.manifest",
            },
        },
        {"test", {"cpp-app/tests/fixtures/parity_fixture_test.cpp"}},
        {"cmake", {"scripts/test-cpp-parity-fixtures.cmake"}},
        {"lineEndings", {".gitattributes"}},
        {"mediaGenerator", {"scripts/generate-media-corpus.sh"}},
    };
    return groups;
}

void append_digest_json(
    std::ostringstream& output,
    std::string_view path,
    const FileDigest& digest,
    std::string_view indent) {
    output << indent << "{\n"
           << indent << "  \"path\": \"" << json_escape(path) << "\",\n"
           << indent << "  \"bytes\": " << digest.bytes << ",\n"
           << indent << "  \"sha256\": \"" << digest.sha256 << "\"\n"
           << indent << "}";
}

void append_named_digest_json(
    std::ostringstream& output,
    std::string_view name,
    std::string_view path,
    const FileDigest& digest,
    std::string_view indent) {
    output << indent << "\"" << name << "\": {\n"
           << indent << "  \"path\": \"" << json_escape(path) << "\",\n"
           << indent << "  \"bytes\": " << digest.bytes << ",\n"
           << indent << "  \"sha256\": \"" << digest.sha256 << "\"\n"
           << indent << "}";
}

}  // namespace

std::string canonical_logical_path(std::string_view path) {
    if (path.empty()) {
        throw std::invalid_argument("logical path cannot be empty");
    }
    if (!is_valid_utf8(std::as_bytes(std::span(path.data(), path.size())))) {
        throw std::invalid_argument("logical path must be valid UTF-8");
    }
    if (path.front() == '/' || path.front() == '\\') {
        throw std::invalid_argument("logical path must be root-relative");
    }

    std::string normalized;
    normalized.reserve(path.size());
    std::string segment;
    const auto append_segment = [&normalized, &segment]() {
        if (segment.empty() || segment == ".") {
            segment.clear();
            return;
        }
        if (segment == "..") {
            throw std::invalid_argument("logical path cannot traverse its root");
        }
        if (!normalized.empty()) {
            normalized.push_back('/');
        }
        normalized += segment;
        segment.clear();
    };
    for (const auto character : path) {
        const auto value = static_cast<unsigned char>(character);
        if (value < 0x20U || value == 0x7fU) {
            throw std::invalid_argument("logical path cannot contain control characters");
        }
        if (character == ':') {
            throw std::invalid_argument("logical path cannot be drive-qualified or contain a scheme");
        }
        if (character == '/' || character == '\\') {
            append_segment();
        } else {
            segment.push_back(character);
        }
    }
    append_segment();
    if (normalized.empty()) {
        throw std::invalid_argument("logical path cannot resolve to empty");
    }
    return normalized;
}

fs::path append_logical_path(const fs::path& root, std::string_view logical_path) {
    const auto canonical = canonical_logical_path(logical_path);
    const auto normalized_root = root.lexically_normal();
    fs::path result = normalized_root;
    std::size_t start = 0;
    while (start < canonical.size()) {
        const auto separator = canonical.find('/', start);
        const auto end = separator == std::string::npos ? canonical.size() : separator;
        result /= path_from_utf8(std::string_view(canonical).substr(start, end - start));
        start = end + 1U;
    }
    const auto relative = result.lexically_relative(normalized_root);
    if (relative.empty() || relative.is_absolute()
        || std::any_of(relative.begin(), relative.end(), [](const auto& component) {
               return component == "..";
           })) {
        throw std::invalid_argument("logical path escapes its physical root");
    }
    return result;
}

bool is_valid_utf8(std::span<const std::byte> bytes) {
    std::size_t index = 0;
    while (index < bytes.size()) {
        const auto first = std::to_integer<std::uint8_t>(bytes[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }

        std::size_t width = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if ((first & 0xe0U) == 0xc0U) {
            width = 2;
            codepoint = first & 0x1fU;
            minimum = 0x80U;
        } else if ((first & 0xf0U) == 0xe0U) {
            width = 3;
            codepoint = first & 0x0fU;
            minimum = 0x800U;
        } else if ((first & 0xf8U) == 0xf0U) {
            width = 4;
            codepoint = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (index + width > bytes.size()) {
            return false;
        }
        for (std::size_t continuation = 1; continuation < width; ++continuation) {
            const auto value = std::to_integer<std::uint8_t>(bytes[index + continuation]);
            if ((value & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (value & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU
            || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
        index += width;
    }
    return true;
}

std::string sha256_bytes(std::span<const std::byte> bytes) {
    Sha256 hash;
    hash.update(bytes);
    return hash.finish();
}

std::string sha256_text(std::string_view text) {
    return sha256_bytes(std::as_bytes(std::span(text.data(), text.size())));
}

FileDigest digest_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot hash fixture file: " + path.string());
    }
    Sha256 hash;
    std::array<char, 64U * 1024U> buffer{};
    std::uint64_t bytes = 0;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = input.gcount();
        if (read > 0) {
            hash.update(std::as_bytes(std::span(buffer.data(), static_cast<std::size_t>(read))));
            bytes += static_cast<std::uint64_t>(read);
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("cannot hash complete fixture file: " + path.string());
    }
    return {.bytes = bytes, .sha256 = hash.finish()};
}

const std::vector<std::string>& document_corpus_paths() {
    static const std::vector<std::string> paths = {
        "Systems 日本語/utf8-text.txt",
        "active-remote.html",
        "blank-500-pages.pdf",
        "malformed-utf8.txt",
        "malformed.docx",
        "malformed.pdf",
        "minimal.docx",
        "oversized-4097-entries.docx",
        "representative.md",
        "unsupported.doc",
    };
    return paths;
}

void generate_document_corpus(const fs::path& output_root) {
    fs::create_directories(output_root);
    write_text_file(
        append_logical_path(output_root, "Systems 日本語/utf8-text.txt"),
        "melearner deterministic UTF-8 fixture\n\nEnglish, 日本語, Ελληνικά, and café.\n");
    write_text_file(
        output_root / "representative.md",
        "# Native document fixture\n\n"
        "Paragraph with **strong text**, *emphasis*, and [a safe link](https://example.invalid/docs).\n\n"
        "- first item\n- second item\n\n"
        "> A quoted fixture line.\n\n"
        "```cpp\nconstexpr int answer = 42;\n```\n\n"
        "| Key | Value |\n| --- | --- |\n| language | C++23 |\n\n"
        "[unsafe link label](javascript:alert('fixture'))\n\n"
        "![remote image](https://example.invalid/remote.png)\n\n"
        "<script>fixtureMustNotRun()</script>\n");
    write_text_file(
        output_root / "active-remote.html",
        "<!doctype html>\n"
        "<html><head>\n"
        "<style>body { display: none; }</style>\n"
        "<script>fixtureMustNotRun()</script>\n"
        "<link rel=\"stylesheet\" href=\"https://example.invalid/remote.css\">\n"
        "</head><body>\n"
        "<h2>Safe heading</h2>\n"
        "<p onclick=\"fixtureMustNotRun()\">Text with <strong>emphasis</strong>.</p>\n"
        "<a href=\"javascript:fixtureMustNotRun()\">unsafe link text</a>\n"
        "<iframe src=\"https://example.invalid/frame\"></iframe>\n"
        "<img src=\"https://example.invalid/tracker.png\" alt=\"remote fixture\">\n"
        "<marquee>unsupported but readable</marquee>\n"
        "</body></html>\n");
    write_minimal_docx(output_root / "minimal.docx");
    write_text_file(
        output_root / "unsupported.doc",
        "MELEARNER UNSUPPORTED .DOC FIXTURE\n"
        "This project-authored file is intentionally not a supported native document.\n");

    constexpr std::array<std::byte, 8> malformed_utf8 = {
        std::byte{'v'},
        std::byte{'a'},
        std::byte{'l'},
        std::byte{'i'},
        std::byte{'d'},
        std::byte{0xff},
        std::byte{0xc0},
        std::byte{0xaf},
    };
    write_binary_file(output_root / "malformed-utf8.txt", malformed_utf8);
    constexpr std::array<std::byte, 11> malformed_docx = {
        std::byte{'P'},
        std::byte{'K'},
        std::byte{0x03},
        std::byte{0x04},
        std::byte{'b'},
        std::byte{'r'},
        std::byte{'o'},
        std::byte{'k'},
        std::byte{'e'},
        std::byte{'n'},
        std::byte{'\n'},
    };
    write_binary_file(output_root / "malformed.docx", malformed_docx);
    write_text_file(
        output_root / "malformed.pdf",
        "%PDF-1.7\n1 0 obj\n<< /Type /Catalog /Pages 99 0 R >>\nendobj\n"
        "% deliberately missing xref, trailer, and EOF\n");
    write_oversized_docx(output_root / "oversized-4097-entries.docx");
    write_text_file(output_root / "blank-500-pages.pdf", minimal_pdf_500_pages());
}

ZipInfo validate_zip(const fs::path& path) {
    const auto bytes = read_binary_file(path);
    if (bytes.size() < 22U) {
        throw std::runtime_error("ZIP is shorter than its end-of-central-directory record");
    }
    const auto eocd_offset = bytes.size() - 22U;
    if (read_le32(bytes, eocd_offset, "EOCD signature") != 0x06054b50U
        || read_le16(bytes, eocd_offset + 4U, "EOCD disk") != 0U
        || read_le16(bytes, eocd_offset + 6U, "EOCD central disk") != 0U
        || read_le16(bytes, eocd_offset + 20U, "EOCD comment") != 0U) {
        throw std::runtime_error("ZIP EOCD is missing or unsupported");
    }
    const auto entries_on_disk = read_le16(bytes, eocd_offset + 8U, "EOCD disk entries");
    const auto entries = read_le16(bytes, eocd_offset + 10U, "EOCD entries");
    const auto central_bytes = read_le32(bytes, eocd_offset + 12U, "EOCD central bytes");
    const auto central_offset = read_le32(bytes, eocd_offset + 16U, "EOCD central offset");
    if (entries_on_disk != entries
        || static_cast<std::uint64_t>(central_offset) + central_bytes != eocd_offset) {
        throw std::runtime_error("ZIP central directory bounds or entry counts are inconsistent");
    }

    std::size_t central_cursor = central_offset;
    std::size_t expected_local_offset = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t stored_entries = 0;
    std::uint64_t deflated_entries = 0;
    std::set<std::string> names;
    for (std::uint16_t index = 0; index < entries; ++index) {
        if (read_le32(bytes, central_cursor, "central signature") != 0x02014b50U) {
            throw std::runtime_error("ZIP central entry signature is invalid");
        }
        const auto flags = read_le16(bytes, central_cursor + 8U, "central flags");
        const auto method = read_le16(bytes, central_cursor + 10U, "central method");
        const auto expected_crc = read_le32(bytes, central_cursor + 16U, "central CRC");
        const auto compressed = read_le32(bytes, central_cursor + 20U, "central compressed size");
        const auto uncompressed = read_le32(bytes, central_cursor + 24U, "central size");
        const auto name_length = read_le16(bytes, central_cursor + 28U, "central name length");
        const auto extra_length = read_le16(bytes, central_cursor + 30U, "central extra length");
        const auto comment_length = read_le16(bytes, central_cursor + 32U, "central comment length");
        const auto disk_start = read_le16(bytes, central_cursor + 34U, "central disk start");
        const auto local_offset = read_le32(bytes, central_cursor + 42U, "central local offset");
        const auto central_entry_bytes = 46U + static_cast<std::size_t>(name_length)
            + static_cast<std::size_t>(extra_length) + static_cast<std::size_t>(comment_length);
        if (central_cursor > eocd_offset || eocd_offset - central_cursor < central_entry_bytes
            || flags != 0x0800U || (method != 0U && method != 8U) || disk_start != 0U) {
            throw std::runtime_error("ZIP central entry fields are invalid");
        }
        const std::string name(
            reinterpret_cast<const char*>(bytes.data() + central_cursor + 46U),
            name_length);
        if (name.empty() || name.contains('\\') || name.starts_with('/')
            || canonical_logical_path(name) != name || !names.insert(name).second) {
            throw std::runtime_error("ZIP entry name is invalid or duplicated");
        }
        if (local_offset != expected_local_offset
            || read_le32(bytes, local_offset, "local signature") != 0x04034b50U
            || read_le16(bytes, local_offset + 6U, "local flags") != flags
            || read_le16(bytes, local_offset + 8U, "local method") != method
            || read_le32(bytes, local_offset + 14U, "local CRC") != expected_crc
            || read_le32(bytes, local_offset + 18U, "local compressed size") != compressed
            || read_le32(bytes, local_offset + 22U, "local size") != uncompressed) {
            throw std::runtime_error("ZIP local entry does not match its central entry");
        }
        const auto local_name_length = read_le16(bytes, local_offset + 26U, "local name length");
        const auto local_extra_length = read_le16(bytes, local_offset + 28U, "local extra length");
        const auto data_offset = static_cast<std::size_t>(local_offset) + 30U
            + static_cast<std::size_t>(local_name_length) + static_cast<std::size_t>(local_extra_length);
        if (local_name_length != name_length || data_offset > central_offset
            || static_cast<std::uint64_t>(data_offset) + compressed > central_offset) {
            throw std::runtime_error("ZIP local entry bounds are invalid");
        }
        const std::string local_name(
            reinterpret_cast<const char*>(bytes.data() + local_offset + 30U),
            local_name_length);
        if (local_name != name) {
            throw std::runtime_error("ZIP local and central names differ");
        }
        const auto encoded = std::span<const std::byte>(bytes).subspan(data_offset, compressed);
        std::vector<std::byte> inflated;
        std::span<const std::byte> contents;
        if (method == 0U) {
            if (compressed != uncompressed) {
                throw std::runtime_error("stored ZIP entry sizes disagree");
            }
            ++stored_entries;
            contents = encoded;
        } else {
            ++deflated_entries;
            inflated = inflate_stored_blocks(encoded, uncompressed);
            contents = inflated;
        }
        if (contents.size() != uncompressed || crc32(contents) != expected_crc) {
            throw std::runtime_error("ZIP entry CRC does not match its stored bytes");
        }
        expected_local_offset = data_offset + compressed;
        payload_bytes += uncompressed;
        central_cursor += central_entry_bytes;
    }
    if (central_cursor != eocd_offset || expected_local_offset != central_offset) {
        throw std::runtime_error("ZIP local or central directory has trailing or missing bytes");
    }
    return {
        .entries = entries,
        .stored_entries = stored_entries,
        .deflated_entries = deflated_entries,
        .payload_bytes = payload_bytes,
    };
}

PdfXrefInfo validate_pdf_xref(const fs::path& path) {
    const auto raw = read_binary_file(path);
    const std::string pdf(reinterpret_cast<const char*>(raw.data()), raw.size());
    if (!pdf.starts_with("%PDF-1.7\n") || !pdf.ends_with("%%EOF\n")) {
        throw std::runtime_error("PDF header or EOF marker is invalid");
    }
    const auto parse_decimal = [&pdf](std::size_t start, std::size_t end, std::string_view field) {
        if (start >= end || end > pdf.size()) {
            throw std::runtime_error("PDF " + std::string(field) + " is missing");
        }
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(pdf.data() + start, pdf.data() + end, value);
        if (parsed.ec != std::errc{} || parsed.ptr != pdf.data() + end) {
            throw std::runtime_error("PDF " + std::string(field) + " is not decimal");
        }
        return value;
    };
    const auto line = [&pdf](std::size_t& cursor) -> std::string_view {
        const auto end = pdf.find('\n', cursor);
        if (end == std::string::npos) {
            throw std::runtime_error("PDF line is unterminated");
        }
        const auto value = std::string_view(pdf).substr(cursor, end - cursor);
        cursor = end + 1U;
        return value;
    };

    const auto startxref_marker = pdf.rfind("startxref\n");
    if (startxref_marker == std::string::npos) {
        throw std::runtime_error("PDF startxref marker is missing");
    }
    auto startxref_value = startxref_marker + std::string_view("startxref\n").size();
    const auto startxref_end = pdf.find('\n', startxref_value);
    const auto xref_offset_value = parse_decimal(startxref_value, startxref_end, "startxref");
    if (xref_offset_value >= startxref_marker || xref_offset_value > pdf.size()) {
        throw std::runtime_error("PDF startxref points outside the xref table");
    }
    auto cursor = static_cast<std::size_t>(xref_offset_value);
    if (line(cursor) != "xref") {
        throw std::runtime_error("PDF startxref does not resolve to xref");
    }
    const auto subsection = line(cursor);
    const auto separator = subsection.find(' ');
    if (separator == std::string_view::npos || subsection.substr(0, separator) != "0") {
        throw std::runtime_error("PDF xref subsection must start at object zero");
    }
    const auto row_count = parse_decimal(
        static_cast<std::size_t>(subsection.data() - pdf.data()) + separator + 1U,
        static_cast<std::size_t>(subsection.data() - pdf.data()) + subsection.size(),
        "xref row count");
    if (row_count < 2U || row_count > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("PDF xref row count is invalid");
    }

    std::uint64_t previous_offset = 0;
    for (std::uint64_t object_id = 0; object_id < row_count; ++object_id) {
        const auto row = line(cursor);
        if (row.size() != 19U || row[10] != ' ' || row[16] != ' ' || row[18] != ' ') {
            throw std::runtime_error("PDF xref row shape is invalid");
        }
        const auto row_start = static_cast<std::size_t>(row.data() - pdf.data());
        const auto object_offset = parse_decimal(row_start, row_start + 10U, "xref offset");
        const auto generation = parse_decimal(row_start + 11U, row_start + 16U, "xref generation");
        if (object_id == 0U) {
            if (object_offset != 0U || generation != 65'535U || row[17] != 'f') {
                throw std::runtime_error("PDF free xref row is invalid");
            }
            continue;
        }
        if (generation != 0U || row[17] != 'n' || object_offset <= previous_offset
            || object_offset >= xref_offset_value) {
            throw std::runtime_error("PDF in-use xref row is invalid");
        }
        const auto object_prefix = std::to_string(object_id) + " 0 obj\n";
        if (std::string_view(pdf).substr(
                static_cast<std::size_t>(object_offset), object_prefix.size())
            != object_prefix) {
            throw std::runtime_error("PDF xref offset does not resolve to its object");
        }
        previous_offset = object_offset;
    }
    const auto trailer = line(cursor);
    if (trailer != "trailer") {
        throw std::runtime_error("PDF trailer marker is missing after xref rows");
    }
    const auto trailer_dictionary = line(cursor);
    const auto expected_size = "/Size " + std::to_string(row_count);
    if (!trailer_dictionary.contains(expected_size) || !trailer_dictionary.contains("/Root 1 0 R")) {
        throw std::runtime_error("PDF trailer dictionary is inconsistent with xref");
    }

    const auto count_marker = pdf.find("/Count ");
    if (count_marker == std::string::npos) {
        throw std::runtime_error("PDF page count is missing");
    }
    const auto count_start = count_marker + std::string_view("/Count ").size();
    const auto count_end = pdf.find(' ', count_start);
    const auto pages = parse_decimal(count_start, count_end, "page count");
    std::uint64_t page_objects = 0;
    std::size_t page_cursor = 0;
    while ((page_cursor = pdf.find("/Type /Page ", page_cursor)) != std::string::npos) {
        ++page_objects;
        page_cursor += std::string_view("/Type /Page ").size();
    }
    if (pages != page_objects || row_count != pages + 3U) {
        throw std::runtime_error("PDF page objects, page count, and xref size disagree");
    }
    return {
        .objects = row_count - 1U,
        .pages = pages,
        .xref_offset = xref_offset_value,
    };
}

std::string build_manifest_v2(const fs::path& repo_root, const GenerationResult& logical_result) {
    if (logical_result.counts.courses != 1'000U || logical_result.counts.lessons != 100'000U
        || logical_result.counts.activity_dates != 84U || logical_result.counts.notes != 201U
        || logical_result.peak_buffered_records > 256U || logical_result.expected.sha256.size() != 64U
        || logical_result.scenario.sha256.size() != 64U || logical_result.report.sha256.size() != 64U
        || logical_result.logical_bundle_sha256.size() != 64U) {
        throw std::invalid_argument("manifest requires a complete canonical logical generation");
    }

    const auto fixtures = collect_manifest_fixtures(repo_root);
    std::ostringstream output;
    output << "{\n"
           << "  \"version\": 2,\n"
           << "  \"selfExclusion\": {\n"
           << "    \"path\": \"fixtures/parity/fixture-manifest-v2.json\",\n"
           << "    \"reason\": \"A manifest cannot include its own digest.\"\n"
           << "  },\n"
           << "  \"recipe\": {\n"
           << "    \"id\": \"cpp-parity-recipe-v1\",\n"
           << "    \"recordBatchLimit\": 256,\n"
           << "    \"counts\": {\n"
           << "      \"courses\": 1000,\n"
           << "      \"lessons\": 100000,\n"
           << "      \"retainedMissingCourses\": 2,\n"
           << "      \"activityDates\": 84,\n"
           << "      \"notes\": 201\n"
           << "    },\n"
           << "    \"pageShapes\": {\n"
           << "      \"courses\": [128, 128, 128, 128, 128, 128, 128, 104],\n"
           << "      \"largeCourseLessons\": [256, 256, 256, 232],\n"
           << "      \"notes\": [100, 100, 1]\n"
           << "    },\n"
           << "    \"logicalOutputs\": {\n"
           << "      \"format\": \"UTF-8 LF; expected/scenario NDJSON v1 and generation report JSON v1\",\n";
    append_named_digest_json(
        output, "expected", "expected-v1.ndjson", logical_result.expected, "      ");
    output << ",\n";
    append_named_digest_json(
        output, "scenario", "scenario-v1.ndjson", logical_result.scenario, "      ");
    output << ",\n";
    append_named_digest_json(
        output, "report", "generation-report-v1.json", logical_result.report, "      ");
    output << ",\n"
           << "      \"bundleSha256\": \"" << logical_result.logical_bundle_sha256 << "\"\n"
           << "    }\n"
           << "  },\n"
           << "  \"mediaOracle\": {\n"
           << "    \"h264Aac\": {\"sha256\": \"ca3861d477f4dc44d0e546405033eb1881ab74322ccf9f0953702b8ad7d5a4b6\", \"videoTracks\": 1, \"audioTracks\": 1},\n"
           << "    \"multiAudioChapters\": {\"sha256\": \"ca1a34ec19424b0ff45236e6964ccfd05409c67d1d462d6b3b78a6b019cc6cda\", \"videoTracks\": 1, \"audioTracks\": 2, \"audioLanguages\": [\"eng\", \"jpn\"], \"chapters\": 2},\n"
           << "    \"hevcMain10\": {\"sha256\": \"ccb2ea5b5657544b3ab4e59862943b8842c22f013ff7fffb3d3e096cde6011e6\", \"videoTracks\": 1, \"profile\": \"main10\"},\n"
           << "    \"srt\": {\"sha256\": \"574c5b8073daead36a209bd21a65a9d3454cfde9d1c29d79a883114e05ba2b9b\", \"language\": \"en\", \"cues\": 2},\n"
           << "    \"vtt\": {\"sha256\": \"d83a4c223d6684e3b5658c5ae369b6878ec2e21d4a5ab9309e8bd90db183b282\", \"language\": \"ja\", \"cues\": 2},\n"
           << "    \"chapterMetadata\": {\"sha256\": \"bbe2b6850795551c5da0011d6199de9729de4bb4ccc8a4a9b8d83f0620dc90fb\", \"chapters\": 2, \"durationMilliseconds\": 2000}\n"
           << "  },\n"
           << "  \"sourceHashes\": {\n";

    const auto& source_groups = manifest_source_groups();
    for (std::size_t group_index = 0; group_index < source_groups.size(); ++group_index) {
        const auto& group = source_groups[group_index];
        output << "    \"" << group.name << "\": [\n";
        for (std::size_t source_index = 0; source_index < group.paths.size(); ++source_index) {
            const auto path = group.paths[source_index];
            append_digest_json(output, path, digest_file(repo_root / path), "      ");
            output << (source_index + 1U == group.paths.size() ? "\n" : ",\n");
        }
        output << "    ]" << (group_index + 1U == source_groups.size() ? "\n" : ",\n");
    }
    output << "  },\n"
           << "  \"files\": [\n";
    for (std::size_t index = 0; index < fixtures.size(); ++index) {
        const auto& fixture = fixtures[index];
        output << "    {\n"
               << "      \"path\": \"" << json_escape(fixture.path) << "\",\n"
               << "      \"bytes\": " << fixture.digest.bytes << ",\n"
               << "      \"sha256\": \"" << fixture.digest.sha256 << "\",\n"
               << "      \"role\": \"" << fixture.role << "\",\n"
               << "      \"provenance\": {\n"
               << "        \"project\": \"melearner\",\n"
               << "        \"kind\": \"" << fixture.provenance_kind << "\",\n"
               << "        \"license\": \"MIT\",\n"
               << "        \"recipe\": ";
        if (fixture.recipe.has_value()) {
            output << "\"" << json_escape(*fixture.recipe) << "\"\n";
        } else {
            output << "null\n";
        }
        output << "      }\n"
               << "    }" << (index + 1U == fixtures.size() ? "\n" : ",\n");
    }
    output << "  ]\n"
           << "}\n";
    return output.str();
}

void write_manifest_v2(const fs::path& repo_root, const GenerationResult& logical_result) {
    write_text_file(
        repo_root / "fixtures/parity/fixture-manifest-v2.json",
        build_manifest_v2(repo_root, logical_result));
}

}  // namespace melearner::fixtures
