#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace melearner::fixtures {

enum class PhysicalMode {
    none,
    smoke,
    full,
};

struct GenerationOptions {
    std::filesystem::path output_root;
    PhysicalMode physical_mode = PhysicalMode::none;
};

struct FixtureCounts {
    std::uint64_t courses = 0;
    std::uint64_t lessons = 0;
    std::uint64_t activity_dates = 0;
    std::uint64_t notes = 0;
};

struct FileDigest {
    std::uint64_t bytes = 0;
    std::string sha256;

    [[nodiscard]] bool operator==(const FileDigest&) const = default;
};

struct GenerationResult {
    FixtureCounts counts;
    std::size_t peak_buffered_records = 0;
    std::uint64_t physical_lessons_created = 0;
    std::uint64_t physical_lessons_present = 0;
    std::uint64_t physical_regular_files_present = 0;
    std::uint64_t physical_bytes_present = 0;
    FileDigest expected;
    FileDigest scenario;
    FileDigest report;
    std::string logical_bundle_sha256;
};

[[nodiscard]] GenerationResult generate_recipe_v1(const GenerationOptions& options);

[[nodiscard]] std::string canonical_logical_path(std::string_view path);
[[nodiscard]] std::filesystem::path append_logical_path(
    const std::filesystem::path& root,
    std::string_view logical_path);
[[nodiscard]] bool is_valid_utf8(std::span<const std::byte> bytes);

[[nodiscard]] std::string sha256_bytes(std::span<const std::byte> bytes);
[[nodiscard]] std::string sha256_text(std::string_view text);
[[nodiscard]] FileDigest digest_file(const std::filesystem::path& path);

void generate_document_corpus(const std::filesystem::path& output_root);
[[nodiscard]] const std::vector<std::string>& document_corpus_paths();

struct ZipInfo {
    std::uint64_t entries = 0;
    std::uint64_t stored_entries = 0;
    std::uint64_t deflated_entries = 0;
    std::uint64_t payload_bytes = 0;
};

struct PdfXrefInfo {
    std::uint64_t objects = 0;
    std::uint64_t pages = 0;
    std::uint64_t xref_offset = 0;
};

[[nodiscard]] ZipInfo validate_zip(const std::filesystem::path& path);
[[nodiscard]] PdfXrefInfo validate_pdf_xref(const std::filesystem::path& path);

[[nodiscard]] std::string build_manifest_v2(
    const std::filesystem::path& repo_root,
    const GenerationResult& logical_result);
void write_manifest_v2(
    const std::filesystem::path& repo_root,
    const GenerationResult& logical_result);

}  // namespace melearner::fixtures
