#include "parity_fixture.hpp"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using melearner::fixtures::GenerationResult;
using melearner::fixtures::PhysicalMode;

void print_usage(std::ostream& output) {
    output
        << "usage:\n"
        << "  parity_fixture generate <output-root> [none|smoke|full]\n"
        << "  parity_fixture documents <output-root>\n"
        << "  parity_fixture manifest <repo-root> <empty-logical-output-root>\n";
}

[[nodiscard]] PhysicalMode parse_mode(std::string_view value) {
    if (value == "none") {
        return PhysicalMode::none;
    }
    if (value == "smoke") {
        return PhysicalMode::smoke;
    }
    if (value == "full") {
        return PhysicalMode::full;
    }
    throw std::invalid_argument("physical mode must be none, smoke, or full");
}

void print_generation(const GenerationResult& result) {
    std::cout << "courses=" << result.counts.courses << " lessons=" << result.counts.lessons
              << " dates=" << result.counts.activity_dates
              << " peakBufferedRecords=" << result.peak_buffered_records << '\n'
              << "expected bytes=" << result.expected.bytes << " sha256=" << result.expected.sha256
              << '\n'
              << "scenario bytes=" << result.scenario.bytes << " sha256=" << result.scenario.sha256
              << '\n'
              << "report bytes=" << result.report.bytes << " sha256=" << result.report.sha256 << '\n'
              << "bundle sha256=" << result.logical_bundle_sha256 << '\n'
              << "physical lessonsCreated=" << result.physical_lessons_created
              << " lessonsPresent=" << result.physical_lessons_present
              << " regularFilesPresent=" << result.physical_regular_files_present
              << " bytesPresent=" << result.physical_bytes_present << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2 || std::string_view(argv[1]) == "--help") {
            print_usage(std::cout);
            return argc < 2 ? 2 : 0;
        }

        const std::string_view command(argv[1]);
        if (command == "generate") {
            if (argc < 3 || argc > 4) {
                print_usage(std::cerr);
                return 2;
            }
            const auto mode = argc == 4 ? parse_mode(argv[3]) : PhysicalMode::none;
            const auto result = melearner::fixtures::generate_recipe_v1({
                .output_root = std::filesystem::path(argv[2]),
                .physical_mode = mode,
            });
            print_generation(result);
            return 0;
        }

        if (command == "documents") {
            if (argc != 3) {
                print_usage(std::cerr);
                return 2;
            }
            melearner::fixtures::generate_document_corpus(std::filesystem::path(argv[2]));
            for (const auto& path : melearner::fixtures::document_corpus_paths()) {
                const auto digest = melearner::fixtures::digest_file(
                    melearner::fixtures::append_logical_path(argv[2], path));
                std::cout << path << " bytes=" << digest.bytes << " sha256=" << digest.sha256 << '\n';
            }
            return 0;
        }

        if (command == "manifest") {
            if (argc != 4) {
                print_usage(std::cerr);
                return 2;
            }
            const auto result = melearner::fixtures::generate_recipe_v1({
                .output_root = std::filesystem::path(argv[3]),
                .physical_mode = PhysicalMode::none,
            });
            melearner::fixtures::write_manifest_v2(std::filesystem::path(argv[2]), result);
            print_generation(result);
            return 0;
        }

        print_usage(std::cerr);
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "parity fixture error: " << error.what() << '\n';
        return 1;
    }
}
