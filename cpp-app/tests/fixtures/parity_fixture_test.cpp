#include "parity_fixture.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using melearner::fixtures::FileDigest;
using melearner::fixtures::GenerationResult;
using melearner::fixtures::PhysicalMode;

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw TestFailure(std::string(message));
    }
}

void require_equal(std::uint64_t actual, std::uint64_t expected, std::string_view message) {
    if (actual != expected) {
        throw TestFailure(
            std::string(message) + ": expected " + std::to_string(expected) + ", got "
            + std::to_string(actual));
    }
}

void require_contains(std::string_view value, std::string_view expected, std::string_view message) {
    if (!value.contains(expected)) {
        throw TestFailure(std::string(message) + ": missing `" + std::string(expected) + "`");
    }
}

template <typename Callable>
void require_throws(Callable&& callable, std::string_view message) {
    try {
        std::forward<Callable>(callable)();
    } catch (const std::exception&) {
        return;
    }
    throw TestFailure(std::string(message));
}

class TestRunner final {
public:
    template <typename Callable>
    void run(std::string_view name, Callable&& callable) {
        try {
            std::forward<Callable>(callable)();
            ++passed_;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed_;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }

    [[nodiscard]] int result() const noexcept {
        return failed_ == 0 ? 0 : 1;
    }

    [[nodiscard]] std::size_t passed() const noexcept {
        return passed_;
    }

    [[nodiscard]] std::size_t failed() const noexcept {
        return failed_;
    }

private:
    std::size_t passed_ = 0;
    std::size_t failed_ = 0;
};

class Workspace final {
public:
    Workspace(const fs::path& repo_root, fs::path root) {
        const auto owner = fs::weakly_canonical(repo_root / ".tmp/cpp-parity-v1");
        root_ = fs::weakly_canonical(std::move(root));
        const auto relative = root_.lexically_relative(owner);
        if (root_ == owner || relative.empty() || relative.is_absolute()
            || std::any_of(relative.begin(), relative.end(), [](const auto& component) {
                   return component == "..";
               })) {
            throw TestFailure("workspace must be a strict descendant of <repo>/.tmp/cpp-parity-v1");
        }
        if (fs::exists(root_)) {
            if (!fs::is_directory(root_) || !fs::is_empty(root_)) {
                throw TestFailure("workspace must be absent or empty at entry");
            }
        } else {
            fs::create_directories(root_);
        }
        owned_ = true;
    }

    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    ~Workspace() {
        if (owned_) {
            std::error_code error;
            fs::remove_all(root_, error);
        }
    }

    [[nodiscard]] const fs::path& root() const noexcept {
        return root_;
    }

private:
    fs::path root_;
    bool owned_ = false;
};

[[nodiscard]] std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw TestFailure("cannot read " + path.string());
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw TestFailure("cannot measure " + path.string());
    }
    const auto size = static_cast<std::size_t>(end);
    std::vector<std::byte> bytes(size);
    input.seekg(0);
    if (size != 0U) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    }
    if (!input) {
        throw TestFailure("cannot read complete file " + path.string());
    }
    return bytes;
}

[[nodiscard]] std::string read_text(const fs::path& path) {
    const auto bytes = read_bytes(path);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void write_text(const fs::path& path, std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw TestFailure("cannot write " + path.string());
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        throw TestFailure("cannot finish " + path.string());
    }
    output.close();
    if (output.fail()) {
        throw TestFailure("cannot close " + path.string());
    }
}

void write_bytes(const fs::path& path, std::span<const std::byte> contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw TestFailure("cannot write " + path.string());
    }
    if (!contents.empty()) {
        output.write(
            reinterpret_cast<const char*>(contents.data()),
            static_cast<std::streamsize>(contents.size()));
    }
    if (!output) {
        throw TestFailure("cannot finish " + path.string());
    }
    output.close();
    if (output.fail()) {
        throw TestFailure("cannot close " + path.string());
    }
}

void append_text(const fs::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) {
        throw TestFailure("cannot append " + path.string());
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        throw TestFailure("cannot finish append to " + path.string());
    }
    output.close();
    if (output.fail()) {
        throw TestFailure("cannot close appended file " + path.string());
    }
}

[[nodiscard]] std::string path_utf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] bool files_equal(const fs::path& left, const fs::path& right) {
    if (fs::file_size(left) != fs::file_size(right)) {
        return false;
    }
    std::ifstream left_input(left, std::ios::binary);
    std::ifstream right_input(right, std::ios::binary);
    if (!left_input || !right_input) {
        throw TestFailure("cannot compare generated files");
    }
    std::array<char, 64U * 1024U> left_buffer{};
    std::array<char, 64U * 1024U> right_buffer{};
    while (left_input && right_input) {
        left_input.read(left_buffer.data(), static_cast<std::streamsize>(left_buffer.size()));
        right_input.read(right_buffer.data(), static_cast<std::streamsize>(right_buffer.size()));
        if (left_input.gcount() != right_input.gcount()) {
            return false;
        }
        if (!std::equal(
                left_buffer.begin(),
                left_buffer.begin() + left_input.gcount(),
                right_buffer.begin())) {
            return false;
        }
    }
    return left_input.eof() && right_input.eof();
}

[[nodiscard]] bool file_contains(const fs::path& path, std::string_view needle) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw TestFailure("cannot search " + path.string());
    }
    std::string line;
    while (std::getline(input, line)) {
        if (line.contains(needle)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::size_t count_substring(std::string_view value, std::string_view needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = value.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

[[nodiscard]] std::optional<std::string> string_field(
    std::string_view record,
    std::string_view name) {
    const auto prefix = "\"" + std::string(name) + "\":\"";
    const auto start = record.find(prefix);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    const auto value_start = start + prefix.size();
    const auto end = record.find('"', value_start);
    if (end == std::string_view::npos) {
        throw TestFailure("unterminated string field in NDJSON record");
    }
    return std::string(record.substr(value_start, end - value_start));
}

[[nodiscard]] std::uint64_t unsigned_field(std::string_view record, std::string_view name) {
    const auto prefix = "\"" + std::string(name) + "\":";
    const auto start = record.find(prefix);
    if (start == std::string_view::npos) {
        throw TestFailure("missing unsigned field " + std::string(name));
    }
    const auto value_start = start + prefix.size();
    std::uint64_t value = 0;
    const auto result = std::from_chars(
        record.data() + static_cast<std::ptrdiff_t>(value_start),
        record.data() + static_cast<std::ptrdiff_t>(record.size()),
        value);
    if (result.ec != std::errc{}) {
        throw TestFailure("invalid unsigned field " + std::string(name));
    }
    return value;
}

[[nodiscard]] std::size_t course_index_from_id(std::string_view id) {
    constexpr std::string_view prefix = "course-";
    if (!id.starts_with(prefix)) {
        throw TestFailure("invalid Course ID in fixture");
    }
    std::size_t index = 0;
    const auto digits = id.substr(prefix.size());
    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), index);
    if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size() || index >= 1'000U) {
        throw TestFailure("invalid Course index in fixture");
    }
    return index;
}

[[nodiscard]] std::string zero_pad(std::size_t value, std::size_t width) {
    std::array<char, 32> digits{};
    const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    if (result.ec != std::errc{}) {
        throw TestFailure("cannot format expected fixture value");
    }
    const auto length = static_cast<std::size_t>(result.ptr - digits.data());
    std::string output(width - length, '0');
    output.append(digits.data(), result.ptr);
    return output;
}

[[nodiscard]] std::string expected_activity_date(std::size_t index) {
    if (index < 29U) {
        return "2026-06-" + zero_pad(index + 2U, 2);
    }
    if (index < 60U) {
        return "2026-07-" + zero_pad(index - 28U, 2);
    }
    return "2026-08-" + zero_pad(index - 59U, 2);
}

[[nodiscard]] std::uint64_t regular_file_count(const fs::path& root) {
    std::uint64_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] std::uint64_t regular_file_bytes(const fs::path& root) {
    std::uint64_t bytes = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            bytes += entry.file_size();
        }
    }
    return bytes;
}

void copy_tree(const fs::path& source, const fs::path& destination) {
    for (const auto& entry : fs::recursive_directory_iterator(source)) {
        const auto relative = fs::relative(entry.path(), source);
        const auto target = destination / relative;
        if (entry.is_directory()) {
            fs::create_directories(target);
        } else if (entry.is_regular_file()) {
            fs::create_directories(target.parent_path());
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
        }
    }
}

void copy_manifest_sources(const fs::path& source_repo, const fs::path& destination_repo) {
    constexpr std::array<std::string_view, 9> source_paths = {
        ".gitattributes",
        "cpp-app/tests/fixtures/parity_fixture.hpp",
        "cpp-app/tests/fixtures/parity_fixture.cpp",
        "cpp-app/tests/fixtures/parity_recipe_v1.cpp",
        "cpp-app/tests/fixtures/parity_fixture_main.cpp",
        "cpp-app/tests/fixtures/parity_fixture_test.cpp",
        "cpp-app/tests/fixtures/parity_fixture.manifest",
        "scripts/test-cpp-parity-fixtures.cmake",
        "scripts/generate-media-corpus.sh",
    };
    for (const auto path : source_paths) {
        const auto target = destination_repo / path;
        fs::create_directories(target.parent_path());
        fs::copy_file(source_repo / path, target, fs::copy_options::overwrite_existing);
    }
}

void require_digest(const FileDigest& digest, std::uint64_t minimum_bytes, std::string_view name) {
    require(digest.bytes >= minimum_bytes, std::string(name) + " is unexpectedly small");
    require(digest.sha256.size() == 64U, std::string(name) + " SHA-256 length is invalid");
    require(
        std::all_of(digest.sha256.begin(), digest.sha256.end(), [](char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        }),
        std::string(name) + " SHA-256 is not lowercase hexadecimal");
}

void require_generation_equal(const GenerationResult& left, const GenerationResult& right) {
    require_equal(left.counts.courses, right.counts.courses, "repeat Course count");
    require_equal(left.counts.lessons, right.counts.lessons, "repeat Lesson count");
    require_equal(left.counts.activity_dates, right.counts.activity_dates, "repeat date count");
    require_equal(left.counts.notes, right.counts.notes, "repeat note count");
    require_equal(left.peak_buffered_records, right.peak_buffered_records, "repeat peak buffer");
    require(left.expected == right.expected, "expected output digest changed between roots");
    require(left.scenario == right.scenario, "scenario output digest changed between roots");
    require(left.report == right.report, "report output digest changed between roots");
    require(
        left.logical_bundle_sha256 == right.logical_bundle_sha256,
        "logical bundle hash changed between roots");
}

void verify_scenario_contract(const fs::path& scenario_path) {
    std::ifstream input(scenario_path, std::ios::binary);
    if (!input) {
        throw TestFailure("cannot read generated scenario");
    }

    std::uint64_t lines = 0;
    std::uint64_t headers = 0;
    std::uint64_t courses = 0;
    std::uint64_t lessons = 0;
    std::uint64_t dates = 0;
    std::uint64_t notes = 0;
    std::uint64_t identity_cases = 0;
    std::uint64_t remove_operations = 0;
    std::uint64_t path_cases = 0;
    std::uint64_t retained_missing = 0;
    std::uint64_t duplicate_marker_courses = 0;
    std::uint64_t duplicate_fingerprint_courses = 0;
    std::uint64_t ambiguous_lessons = 0;
    bool saw_non_ascii_path = false;
    bool saw_long_path = false;
    bool saw_long_relative_path = false;
    std::array<std::uint64_t, 1'000> course_lesson_counts{};
    std::array<std::string, 1'000> course_paths{};
    std::array<std::string, 1'000> course_sections{};
    std::array<std::string, 1'000> fingerprint_bases{};
    std::array<bool, 4> identity_seen{};
    std::string line;
    while (std::getline(input, line)) {
        ++lines;
        const auto record = string_field(line, "record");
        require(record.has_value(), "scenario record lacks a record type");
        if (*record == "fixture") {
            ++headers;
            require_contains(line, "\"recordBatchLimit\":256", "scenario batch limit");
            continue;
        }
        if (*record == "course") {
            ++courses;
            const auto id = string_field(line, "id");
            const auto logical_path = string_field(line, "logicalPath");
            const auto section_name = string_field(line, "sectionName");
            const auto fingerprint_basis = string_field(line, "fingerprintBasisId");
            require(
                id.has_value() && logical_path.has_value() && section_name.has_value()
                    && fingerprint_basis.has_value(),
                "Course record lacks its physical fingerprint basis");
            const auto index = course_index_from_id(*id);
            course_paths[index] = *logical_path;
            course_sections[index] = *section_name;
            fingerprint_bases[index] = *fingerprint_basis;
            const auto expected_lessons = index == 0U ? 1'000U : (index < 100U ? 100U : 99U);
            require_equal(unsigned_field(line, "lessonCount"), expected_lessons, "Course Lesson formula");
            require(logical_path->front() != '/', "Course logical path is absolute");
            require(!logical_path->contains('/'), "Course is not a direct child of the Root folder");
            require(!logical_path->contains('\\'), "Course logical path contains a backslash");
            require(
                melearner::fixtures::canonical_logical_path(*logical_path) == *logical_path,
                "Course logical path is not canonical");
            saw_non_ascii_path = saw_non_ascii_path || logical_path->contains("日本語");
            saw_long_path = saw_long_path || logical_path->size() > 260U;
            if (line.contains("\"state\":\"retained-missing\"")) {
                ++retained_missing;
            }
            if (line.contains("\"markerIdentity\":\"identity-duplicate-marker\"")) {
                ++duplicate_marker_courses;
            }
            if (*fingerprint_basis == "fingerprint-basis-v1-0004") {
                ++duplicate_fingerprint_courses;
            }
            continue;
        }
        if (*record == "lesson") {
            ++lessons;
            const auto course = string_field(line, "courseId");
            const auto logical_path = string_field(line, "logicalPath");
            const auto relative_path = string_field(line, "relativePath");
            require(
                course.has_value() && logical_path.has_value() && relative_path.has_value(),
                "Lesson record is incomplete");
            const auto course_index = course_index_from_id(*course);
            ++course_lesson_counts.at(course_index);
            require(logical_path->front() != '/', "Lesson logical path is absolute");
            require(!logical_path->contains('\\'), "Lesson logical path contains a backslash");
            require(!relative_path->contains('\\'), "Lesson relative path contains a backslash");
            require(
                logical_path->starts_with(course_paths[course_index] + "/"),
                "Lesson logical path is outside its direct-child Course");
            require(
                relative_path->starts_with(course_sections[course_index] + "/"),
                "Lesson relative path does not use its fingerprint Section basis");
            require(unsigned_field(line, "fileBytes") > 0U, "Lesson fixture is empty");
            saw_long_path = saw_long_path || logical_path->size() > 260U;
            saw_long_relative_path = saw_long_relative_path || relative_path->size() > 260U;
            if (line.contains("\"name\":\"Ambiguous Lesson\"")) {
                ++ambiguous_lessons;
                require_contains(line, "\"fileBytes\":17", "ambiguous Lesson metadata");
            }
            continue;
        }
        if (*record == "activityDate") {
            const auto date = string_field(line, "date");
            require(date.has_value(), "activity record lacks a UTC date");
            require(*date == expected_activity_date(static_cast<std::size_t>(dates)), "activity date gap");
            ++dates;
            continue;
        }
        if (*record == "note") {
            const auto id = string_field(line, "id");
            require(id.has_value(), "note record lacks an ID");
            require(
                *id == "note-" + zero_pad(static_cast<std::size_t>(notes), 3),
                "equal-timestamp notes are not stably ordered by ID");
            require_contains(line, "\"lessonId\":\"lesson-0001-000000\"", "note Lesson scope");
            require_contains(line, "\"timestampSeconds\":42", "note media timestamp");
            require_contains(
                line,
                "\"createdAt\":\"2026-08-24T12:00:00.000Z\"",
                "stable equal note timestamp");
            ++notes;
            continue;
        }
        if (*record == "identityCase") {
            ++identity_cases;
            const auto identity_case = string_field(line, "case");
            require(identity_case.has_value(), "identity case lacks a name");
            if (*identity_case == "duplicate-marker") {
                identity_seen[0] = true;
                require_contains(line, "\"persisted\":[", "duplicate-marker persisted inputs");
                require_contains(line, "\"scanned\":[", "duplicate-marker scanned inputs");
                require_contains(line, "\"warning\":\"duplicate-marker\"", "duplicate-marker warning");
                require_contains(
                    line,
                    "\"markerMatchesIgnored\":true",
                    "duplicate-marker fallback policy");
                require_contains(
                    line,
                    "\"scanId\":\"scan-marker-a\",\"courseId\":\"course-0002\",\"by\":\"fingerprint\"",
                    "duplicate-marker fingerprint match A");
                require_contains(
                    line,
                    "\"scanId\":\"scan-marker-b\",\"courseId\":\"course-0003\",\"by\":\"fingerprint\"",
                    "duplicate-marker fingerprint match B");
            } else if (*identity_case == "duplicate-fingerprint") {
                identity_seen[1] = true;
                require_contains(line, "\"persisted\":[", "duplicate-fingerprint persisted inputs");
                require_contains(line, "\"scanned\":{", "duplicate-fingerprint scanned input");
            } else if (*identity_case == "moved-marker") {
                identity_seen[2] = true;
                require_contains(line, "\"persisted\":{", "moved-marker persisted input");
                require_contains(line, "\"scanned\":{", "moved-marker scanned input");
            } else if (*identity_case == "ambiguous-lesson-metadata") {
                identity_seen[3] = true;
                require_contains(line, "lesson-0007-000000", "ambiguous Lesson candidate A");
                require_contains(line, "lesson-0007-000001", "ambiguous Lesson candidate B");
                require_contains(line, "\"scanned\":{", "ambiguous Lesson scanned input");
                require_contains(line, "Incoming Ambiguous.txt", "incoming ambiguous Lesson path");
            } else {
                throw TestFailure("unexpected identity case");
            }
            continue;
        }
        if (*record == "operation") {
            ++remove_operations;
            require_contains(line, "\"operation\":\"removeCourse\"", "missing Course removal");
            require_contains(line, "\"expectedState\":\"retained-missing\"", "retained state");
            continue;
        }
        if (*record == "pathCase") {
            ++path_cases;
            require_contains(line, "\"case\":\"mixed-separators\"", "mixed separator case");
            require_contains(line, "\"input\":\"Systems 日本語", "mixed separator input");
            require_contains(line, "\"expectedCanonical\":\"Systems 日本語", "canonical output");
            continue;
        }
        throw TestFailure("unexpected scenario record type: " + *record);
    }

    require_equal(lines, 101'293U, "scenario NDJSON records");
    require_equal(headers, 1U, "scenario header records");
    require_equal(courses, 1'000U, "scenario Courses");
    require_equal(lessons, 100'000U, "scenario Lessons");
    require_equal(dates, 84U, "scenario activity dates");
    require_equal(notes, 201U, "scenario notes");
    require_equal(identity_cases, 4U, "scenario identity cases");
    require_equal(remove_operations, 2U, "missing Course removal operations");
    require_equal(path_cases, 1U, "separator canonicalization records");
    require_equal(retained_missing, 2U, "retained missing Courses");
    require_equal(duplicate_marker_courses, 2U, "isolated duplicate-marker pair");
    require_equal(duplicate_fingerprint_courses, 2U, "isolated duplicate-fingerprint pair");
    require_equal(ambiguous_lessons, 2U, "ambiguous Lesson metadata pair");
    require(saw_non_ascii_path, "non-ASCII logical path is missing");
    require(saw_long_path, "logical path over 260 bytes is missing");
    require(saw_long_relative_path, "long path is not inside a Lesson-relative path");
    require(std::all_of(identity_seen.begin(), identity_seen.end(), [](bool value) { return value; }),
            "one or more deliberate identity cases are missing");
    require(
        fingerprint_bases[2] != fingerprint_bases[3]
            && course_sections[2] != course_sections[3],
        "duplicate-marker pair also shares a fingerprint basis");
    require(
        fingerprint_bases[4] == fingerprint_bases[5]
            && course_sections[4] == course_sections[5],
        "deliberate duplicate-fingerprint pair does not share its physical basis");
    std::map<std::string, std::vector<std::size_t>> basis_courses;
    for (std::size_t index = 0; index < fingerprint_bases.size(); ++index) {
        require(!fingerprint_bases[index].empty(), "Course fingerprint basis is empty");
        basis_courses[fingerprint_bases[index]].push_back(index);
    }
    require_equal(basis_courses.size(), 999U, "unique fingerprint basis groups");
    for (const auto& [basis, members] : basis_courses) {
        if (basis == "fingerprint-basis-v1-0004") {
            require(
                members == std::vector<std::size_t>({4U, 5U}),
                "wrong Courses share the deliberate fingerprint basis");
        } else {
            require_equal(members.size(), 1U, "non-deliberate fingerprint basis collision");
        }
    }
    for (std::size_t index = 0; index < course_lesson_counts.size(); ++index) {
        const auto expected = index == 0U ? 1'000U : (index < 100U ? 100U : 99U);
        require_equal(course_lesson_counts[index], expected, "streamed per-Course Lesson count");
    }
}

void verify_expected_contract(const fs::path& expected_path) {
    const auto expected = read_text(expected_path);
    require_contains(
        expected,
        "\"rows\":[128,128,128,128,128,128,128,104]",
        "Course page tail");
    require_contains(expected, "\"rows\":[256,256,256,232]", "large Course Lesson page tail");
    require_contains(expected, "\"rows\":[100,100,1]", "note page tail");
    require_contains(expected, "\"order\":[\"createdAt\",\"id\"]", "stable note ordering");
    require_contains(expected, "\"firstDate\":\"2026-06-02\"", "activity first date");
    require_contains(expected, "\"lastDate\":\"2026-08-24\"", "activity last date");
    require_contains(expected, "\"presentLessonsAfterRemovals\":99802", "final scan Lesson count");
    require_contains(
        expected,
        "\"case\":\"duplicate-marker\",\"outcome\":"
        "\"warn-ignore-marker-and-match-by-fingerprint\"",
        "duplicate-marker expectation");
    require_contains(
        expected,
        "\"case\":\"duplicate-fingerprint\",\"outcome\":\"ambiguous-refuse-reuse\"",
        "duplicate-fingerprint expectation");
    require_equal(count_substring(expected, "\"record\":\"identityExpectation\""), 4U,
                   "identity expectations");
}

void verify_physical_fingerprint_tree(
    const fs::path& library_root,
    const fs::path& scenario_path) {
    std::ifstream input(scenario_path, std::ios::binary);
    if (!input) {
        throw TestFailure("cannot read scenario for physical fingerprint validation");
    }
    std::uint64_t available_courses = 0;
    std::uint64_t missing_courses = 0;
    std::string line;
    while (std::getline(input, line)) {
        if (string_field(line, "record") != std::optional<std::string>("course")) {
            continue;
        }
        const auto logical_path = string_field(line, "logicalPath");
        const auto section_name = string_field(line, "sectionName");
        require(logical_path.has_value() && section_name.has_value(), "Course basis record is incomplete");
        const auto course_root = melearner::fixtures::append_logical_path(library_root, *logical_path);
        if (line.contains("\"state\":\"retained-missing\"")) {
            ++missing_courses;
            require(!fs::exists(course_root), "retained-missing Course exists in final physical tree");
            continue;
        }
        ++available_courses;
        require(fs::is_directory(course_root / *section_name), "Course Section basis is absent physically");
    }
    require_equal(available_courses, 998U, "available physical Courses");
    require_equal(missing_courses, 2U, "missing physical Courses");
    std::uint64_t direct_course_directories = 0;
    for (const auto& entry : fs::directory_iterator(library_root)) {
        require(entry.is_directory(), "Root folder contains a non-Course entry");
        ++direct_course_directories;
    }
    require_equal(direct_course_directories, 998U, "direct physical Course directories");
}

void verify_document_contract(const fs::path& tracked_root, const fs::path& generated_root) {
    melearner::fixtures::generate_document_corpus(generated_root);
    const auto& paths = melearner::fixtures::document_corpus_paths();
    require_equal(paths.size(), 10U, "document corpus path count");
    require_equal(regular_file_count(tracked_root), 10U, "tracked document corpus count");
    require_equal(regular_file_count(generated_root), 10U, "regenerated document corpus count");
    for (const auto& path : paths) {
        const auto tracked = melearner::fixtures::append_logical_path(tracked_root, path);
        const auto generated = melearner::fixtures::append_logical_path(generated_root, path);
        require(fs::is_regular_file(tracked), "tracked document is missing: " + path);
        require(files_equal(tracked, generated), "document bytes are not deterministic: " + path);
    }

    const auto valid_utf8 = read_bytes(melearner::fixtures::append_logical_path(
        tracked_root, "Systems 日本語/utf8-text.txt"));
    const auto malformed_utf8 = read_bytes(tracked_root / "malformed-utf8.txt");
    require(melearner::fixtures::is_valid_utf8(valid_utf8), "valid UTF-8 fixture is invalid");
    require(!melearner::fixtures::is_valid_utf8(malformed_utf8), "malformed UTF-8 was accepted");

    const auto markdown = read_text(tracked_root / "representative.md");
    require_contains(markdown, "| Key | Value |", "Markdown table");
    require_contains(markdown, "javascript:alert", "Markdown unsafe link case");
    require_contains(markdown, "```cpp", "Markdown code fence");
    const auto html = read_text(tracked_root / "active-remote.html");
    require_contains(html, "<script>", "HTML active construct");
    require_contains(html, "<iframe", "HTML remote construct");
    require_contains(html, "javascript:", "HTML unsafe link");

    const auto minimal_zip = melearner::fixtures::validate_zip(
        tracked_root / "minimal.docx");
    require_equal(minimal_zip.entries, 4U, "minimal DOCX ZIP entries");
    require_equal(minimal_zip.stored_entries, 0U, "minimal DOCX stored entries");
    require_equal(minimal_zip.deflated_entries, 4U, "minimal DOCX deflated entries");
    require(minimal_zip.payload_bytes > 0U, "minimal DOCX has no expanded payload");
    const auto oversized_zip = melearner::fixtures::validate_zip(
        tracked_root / "oversized-4097-entries.docx");
    require_equal(oversized_zip.entries, 4'097U, "oversized DOCX ZIP entries");
    require_equal(oversized_zip.stored_entries, 4'097U, "oversized DOCX stored entries");
    require_equal(oversized_zip.deflated_entries, 0U, "oversized DOCX deflated entries");
    require_equal(oversized_zip.payload_bytes, 0U, "oversized DOCX payload bytes");
    require_throws(
        [&] {
            static_cast<void>(melearner::fixtures::validate_zip(
                tracked_root / "malformed.docx"));
        },
        "malformed DOCX passed structural ZIP validation");

    auto corrupt_zip_bytes = read_bytes(tracked_root / "minimal.docx");
    const std::string corrupt_zip_text(
        reinterpret_cast<const char*>(corrupt_zip_bytes.data()), corrupt_zip_bytes.size());
    const auto zip_payload = corrupt_zip_text.find("Fixture chapter");
    require(zip_payload != std::string::npos, "minimal DOCX payload marker is missing");
    corrupt_zip_bytes[zip_payload] ^= std::byte{0x01};
    const auto corrupt_zip = generated_root / "corrupt-crc.docx";
    write_bytes(corrupt_zip, corrupt_zip_bytes);
    require_throws(
        [&] { static_cast<void>(melearner::fixtures::validate_zip(corrupt_zip)); },
        "ZIP validator accepted corrupted stored bytes/CRC");

    const auto pdf = read_text(tracked_root / "blank-500-pages.pdf");
    require_equal(count_substring(pdf, "/Type /Page "), 500U, "PDF page objects");
    require_contains(pdf, "/Count 500", "PDF page count");
    require_contains(pdf, "startxref\n", "PDF xref pointer");
    require(pdf.ends_with("%%EOF\n"), "PDF does not have a deterministic final newline");
    require(!pdf.contains("/Font"), "blank PDF depends on a font");
    require(!pdf.contains("CreationDate"), "blank PDF contains date metadata");
    const auto pdf_info = melearner::fixtures::validate_pdf_xref(
        tracked_root / "blank-500-pages.pdf");
    require_equal(pdf_info.pages, 500U, "validated PDF pages");
    require_equal(pdf_info.objects, 502U, "validated PDF objects");
    require(pdf_info.xref_offset > 0U, "validated PDF xref offset is zero");

    auto corrupt_pdf = pdf;
    const auto xref_header = corrupt_pdf.find("xref\n0 503\n");
    require(xref_header != std::string::npos, "PDF xref header is missing");
    const auto first_in_use_row = xref_header + std::string_view("xref\n0 503\n").size() + 20U;
    corrupt_pdf[first_in_use_row] = '9';
    const auto corrupt_pdf_path = generated_root / "corrupt-xref.pdf";
    write_text(corrupt_pdf_path, corrupt_pdf);
    require_throws(
        [&] { static_cast<void>(melearner::fixtures::validate_pdf_xref(corrupt_pdf_path)); },
        "PDF validator accepted a corrupted xref object offset");

    const auto malformed_pdf = read_text(tracked_root / "malformed.pdf");
    require(!malformed_pdf.contains("xref\n"), "malformed PDF unexpectedly contains an xref");
    require(!malformed_pdf.contains("%%EOF"), "malformed PDF unexpectedly contains EOF");
    require_throws(
        [&] {
            static_cast<void>(melearner::fixtures::validate_pdf_xref(
                tracked_root / "malformed.pdf"));
        },
        "malformed PDF passed xref validation");
    require(
        fs::path("unsupported.doc").extension() == ".doc"
            && read_text(tracked_root / "unsupported.doc").contains("UNSUPPORTED"),
        "unsupported .doc case is missing");
}

void verify_media_contract(const fs::path& repo_root) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 7> media = {{
        {
            "fixtures/parity/media/Systems 日本語/01 H264 AAC.mp4",
            "ca3861d477f4dc44d0e546405033eb1881ab74322ccf9f0953702b8ad7d5a4b6",
        },
        {
            "fixtures/parity/media/02 Multi audio chapters.mkv",
            "ca1a34ec19424b0ff45236e6964ccfd05409c67d1d462d6b3b78a6b019cc6cda",
        },
        {
            "fixtures/parity/media/03 HEVC Main 10.mkv",
            "ccb2ea5b5657544b3ab4e59862943b8842c22f013ff7fffb3d3e096cde6011e6",
        },
        {
            "fixtures/parity/media/01 H264 AAC.en.srt",
            "574c5b8073daead36a209bd21a65a9d3454cfde9d1c29d79a883114e05ba2b9b",
        },
        {
            "fixtures/parity/media/01 H264 AAC.ja.vtt",
            "d83a4c223d6684e3b5658c5ae369b6878ec2e21d4a5ab9309e8bd90db183b282",
        },
        {
            "fixtures/parity/media/chapters.ffmeta",
            "bbe2b6850795551c5da0011d6199de9729de4bb4ccc8a4a9b8d83f0620dc90fb",
        },
        {
            "fixtures/parity/media/corrupt-media.bin",
            "b2b5a4dca09c270d104473bf57dd977e3ee1d93da83a883dcd2255e8c1e63f91",
        },
    }};
    for (const auto& [path, hash] : media) {
        require(
            melearner::fixtures::digest_file(
                melearner::fixtures::append_logical_path(repo_root, path)).sha256
                == hash,
            "frozen media hash changed: " + std::string(path));
    }
    require_equal(
        count_substring(read_text(repo_root / "fixtures/parity/media/01 H264 AAC.en.srt"), " --> "),
        2U,
        "SRT cue count");
    require_equal(
        count_substring(read_text(repo_root / "fixtures/parity/media/01 H264 AAC.ja.vtt"), " --> "),
        2U,
        "VTT cue count");
    require_equal(
        count_substring(read_text(repo_root / "fixtures/parity/media/chapters.ffmeta"), "[CHAPTER]"),
        2U,
        "chapter metadata count");

    const auto manifest_v1 = read_text(repo_root / "fixtures/parity/fixture-manifest-v1.json");
    require_contains(manifest_v1, "\"audioTracks\": 2", "v1 multi-audio oracle");
    require_contains(manifest_v1, "\"chapters\": 2", "v1 chapter oracle");
    require_contains(manifest_v1, "\"version\": \"n8.1.2\"", "pinned ffmpeg provenance");
    require(
        melearner::fixtures::digest_file(repo_root / "scripts/generate-media-corpus.sh").sha256
            == "beebb38dea2eb4792fc8cecd2cad72412362ee5607683114e19ebaefb16591f0",
        "frozen media recipe hash changed");
}

void verify_source_boundary(const fs::path& repo_root, std::string_view manifest) {
    constexpr std::array<std::string_view, 5> fixture_sources = {
        "cpp-app/tests/fixtures/parity_fixture.hpp",
        "cpp-app/tests/fixtures/parity_fixture.cpp",
        "cpp-app/tests/fixtures/parity_recipe_v1.cpp",
        "cpp-app/tests/fixtures/parity_fixture_main.cpp",
        "cpp-app/tests/fixtures/parity_fixture_test.cpp",
    };
    for (const auto source : fixture_sources) {
        require(fs::is_regular_file(repo_root / source), "fixture source is missing");
        require(
            source.starts_with("cpp-app/tests/fixtures/"),
            "fixture source escaped the test-only directory");
    }

    std::uint64_t named_fixture_sources = 0;
    for (const auto& entry : fs::recursive_directory_iterator(repo_root / "cpp-app")) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto extension = entry.path().extension();
        if (extension != ".cpp" && extension != ".hpp") {
            continue;
        }
        const auto relative = path_utf8(fs::relative(entry.path(), repo_root));
        const auto filename = path_utf8(entry.path().filename());
        if (!filename.starts_with("parity_fixture") && !filename.starts_with("parity_recipe")) {
            continue;
        }
        ++named_fixture_sources;
        require(
            relative.starts_with("cpp-app/tests/fixtures/"),
            "T02 C++ source escaped the test-only boundary: " + relative);
    }
    require_equal(named_fixture_sources, 5U, "test-only fixture C++ source count");
    require_contains(manifest, "\"generator\": [", "separate generator source hashes");
    require_contains(manifest, "\"test\": [", "separate test source hashes");
    require_contains(manifest, "\"cmake\": [", "separate CMake source hashes");
    require_contains(manifest, "\"lineEndings\": [", "line-ending policy source hash");
    const auto attributes = read_text(repo_root / ".gitattributes");
    require(!attributes.contains('\r'), ".gitattributes contains CR line endings");
    require_contains(attributes, "cpp-app/tests/fixtures/*.cpp text eol=lf", "C++ LF rule");
    require_contains(attributes, "cpp-app/tests/fixtures/*.manifest text eol=lf", "manifest LF rule");
    require_contains(attributes, "fixtures/parity/*.json text eol=lf", "manifest LF rule");
    require_contains(attributes, "fixtures/parity/documents/** -text", "document binary default");
    require_contains(attributes, "fixtures/parity/media/** -text", "media binary default");
    const auto runner = read_text(repo_root / "scripts/test-cpp-parity-fixtures.cmake");
    require(!runner.contains("install("), "fixture runner adds an install boundary");
    require_contains(runner, "/MANIFESTINPUT:", "MSVC embedded manifest linker input");
    const auto windows_manifest = read_text(
        repo_root / "cpp-app/tests/fixtures/parity_fixture.manifest");
    require_contains(windows_manifest, "<longPathAware", "Windows long-path manifest setting");
    require_contains(windows_manifest, ">true</longPathAware>", "Windows long-path manifest value");
    require(!fs::exists(repo_root / "cpp-app/expected-v1.ndjson"), "generated fixture entered source tree");
}

void verify_manifest_contract(
    const fs::path& repo_root,
    const fs::path& workspace_root,
    const GenerationResult& logical_result) {
    const auto manifest_path = repo_root / "fixtures/parity/fixture-manifest-v2.json";
    const auto tracked = read_text(manifest_path);
    const auto regenerated = melearner::fixtures::build_manifest_v2(repo_root, logical_result);
    require(tracked == regenerated, "manifest v2 does not match exact regeneration");
    require(tracked.ends_with('\n'), "manifest v2 lacks a final newline");
    require(!tracked.contains('\r'), "manifest v2 contains CR line endings");
    require(!tracked.contains('\t'), "manifest v2 contains tab indentation");
    require_contains(tracked, "\"selfExclusion\"", "manifest self-exclusion");
    require_contains(
        tracked,
        "fixtures/parity/fixture-manifest-v2.json",
        "manifest self-exclusion path");
    require_contains(tracked, "\"recordBatchLimit\": 256", "manifest batch limit");
    require_contains(tracked, "\"bundleSha256\":", "manifest logical bundle hash");
    require_contains(tracked, "\"cues\": 2", "manifest subtitle cue metadata");
    require_contains(tracked, "\"audioTracks\": 2", "manifest audio track metadata");
    require_contains(tracked, "\"chapters\": 2", "manifest chapter metadata");

    std::vector<std::string> manifest_paths;
    bool in_files = false;
    std::string line;
    std::ifstream lines(manifest_path, std::ios::binary);
    while (std::getline(lines, line)) {
        if (line == "  \"files\": [") {
            in_files = true;
            continue;
        }
        if (in_files && line == "  ]") {
            break;
        }
        constexpr std::string_view prefix = "      \"path\": \"";
        if (in_files && std::string_view(line).starts_with(prefix)) {
            manifest_paths.push_back(
                line.substr(prefix.size(), line.size() - prefix.size() - 2U));
        }
    }
    require_equal(manifest_paths.size(), 22U, "manifest fixture file count");
    require(std::is_sorted(manifest_paths.begin(), manifest_paths.end()), "manifest paths are not raw UTF-8 sorted");
    require(
        std::none_of(manifest_paths.begin(), manifest_paths.end(), [](const auto& path) {
            return path == "fixtures/parity/fixture-manifest-v2.json";
        }),
        "manifest included itself in the file inventory");
    for (const auto& entry : fs::recursive_directory_iterator(repo_root / "fixtures/parity")) {
        if (!entry.is_regular_file() || entry.path() == manifest_path) {
            continue;
        }
        const auto relative = path_utf8(fs::relative(entry.path(), repo_root));
        require(
            std::binary_search(manifest_paths.begin(), manifest_paths.end(), relative),
            "regular fixture is absent from manifest: " + relative);
    }

    const auto staged_repo = workspace_root / "manifest-negative-repo";
    copy_tree(repo_root / "fixtures/parity", staged_repo / "fixtures/parity");
    copy_manifest_sources(repo_root, staged_repo);
    require(
        melearner::fixtures::build_manifest_v2(staged_repo, logical_result) == tracked,
        "staged manifest inputs changed the generated manifest");

    const auto extra = staged_repo / "fixtures/parity/unlisted-fixture.bin";
    write_text(extra, "unlisted\n");
    require_throws(
        [&] { static_cast<void>(melearner::fixtures::build_manifest_v2(staged_repo, logical_result)); },
        "manifest regeneration accepted an unlisted fixture");
    fs::remove(extra);

    const auto invalid_path = staged_repo / "fixtures/parity/bad\\separator.bin";
    write_text(invalid_path, "bad path\n");
    require_throws(
        [&] { static_cast<void>(melearner::fixtures::build_manifest_v2(staged_repo, logical_result)); },
        "manifest regeneration accepted a non-POSIX fixture path");
    fs::remove(invalid_path);

    append_text(staged_repo / "fixtures/parity/scanner-v1.json", "changed\n");
    require(
        melearner::fixtures::build_manifest_v2(staged_repo, logical_result) != tracked,
        "manifest verification accepted changed fixture bytes/hash");
    fs::copy_file(
        repo_root / "fixtures/parity/scanner-v1.json",
        staged_repo / "fixtures/parity/scanner-v1.json",
        fs::copy_options::overwrite_existing);
    fs::remove(staged_repo / "fixtures/parity/media/01 H264 AAC.en.srt");
    require(
        melearner::fixtures::build_manifest_v2(staged_repo, logical_result) != tracked,
        "manifest verification accepted a missing fixture");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: parity_fixture_test <repo-root> <workspace-root> [--full-materialization]\n";
        return 2;
    }
    const fs::path repo_root(argv[1]);
    Workspace workspace{repo_root, fs::path(argv[2])};
    const bool run_full = argc == 4 && std::string_view(argv[3]) == "--full-materialization";
    if (argc == 4 && !run_full) {
        std::cerr << "unknown test option: " << argv[3] << '\n';
        return 2;
    }

    TestRunner tests;
    GenerationResult first;
    GenerationResult second;
    GenerationResult smoke;
    const auto first_root = workspace.root() / "logical-a";
    const auto second_root = workspace.root() / "logical-b";
    const auto smoke_root = workspace.root() / "physical-smoke";

    tests.run("full logical generation uses fixed counts and bounded records", [&] {
        first = melearner::fixtures::generate_recipe_v1({
            .output_root = first_root,
            .physical_mode = PhysicalMode::none,
        });
        require_equal(first.counts.courses, 1'000U, "generated Courses");
        require_equal(first.counts.lessons, 100'000U, "generated Lessons");
        require_equal(first.counts.activity_dates, 84U, "generated activity dates");
        require_equal(first.counts.notes, 201U, "generated notes");
        require_equal(first.peak_buffered_records, 256U, "peak buffered records");
        require_equal(first.physical_lessons_created, 0U, "default created physical Lessons");
        require_equal(first.physical_lessons_present, 0U, "default present physical Lessons");
        require_digest(first.expected, 1U, "expected-v1.ndjson");
        require_digest(first.scenario, 1'000'000U, "scenario-v1.ndjson");
        require_digest(first.report, 1U, "generation-report-v1.json");
        require_digest({.bytes = 64U, .sha256 = first.logical_bundle_sha256}, 64U, "logical bundle");
        verify_scenario_contract(first_root / "scenario-v1.ndjson");
        verify_expected_contract(first_root / "expected-v1.ndjson");
        const auto report = read_text(first_root / "generation-report-v1.json");
        require_contains(report, "\"peakBufferedRecords\": 256", "reported peak buffer");
        require_contains(report, "\"physicalMode\": \"none\"", "default physical mode");
    });

    tests.run("clean generations are byte-identical and root-independent", [&] {
        second = melearner::fixtures::generate_recipe_v1({
            .output_root = second_root,
            .physical_mode = PhysicalMode::none,
        });
        require_generation_equal(first, second);
        for (const auto name : {
                 "expected-v1.ndjson", "scenario-v1.ndjson", "generation-report-v1.json"}) {
            require(files_equal(first_root / name, second_root / name), "logical output bytes changed");
            require(!file_contains(first_root / name, path_utf8(first_root)), "first temp root leaked");
            require(!file_contains(first_root / name, path_utf8(second_root)), "second temp root leaked");
        }
    });

    tests.run("logical separators canonicalize before physical append", [&] {
        const auto slash = melearner::fixtures::canonical_logical_path(
            "Systems 日本語/Section 0000/Lesson.txt");
        const auto mixed = melearner::fixtures::canonical_logical_path(
            "Systems 日本語//Section 0000\\Lesson.txt");
        require(slash == mixed, "mixed separators did not canonicalize to POSIX");
        require(
            melearner::fixtures::append_logical_path(workspace.root(), slash)
                == melearner::fixtures::append_logical_path(workspace.root(), mixed),
            "canonical logical paths append to different physical paths");
        require_throws(
            [] { static_cast<void>(melearner::fixtures::canonical_logical_path("../escape")); },
            "logical parent traversal was accepted");
        require_throws(
            [] { static_cast<void>(melearner::fixtures::canonical_logical_path("/absolute")); },
            "absolute logical path was accepted");
        require_throws(
            [] { static_cast<void>(melearner::fixtures::canonical_logical_path("C:\\outside")); },
            "Windows drive path with backslashes was accepted");
        require_throws(
            [] { static_cast<void>(melearner::fixtures::canonical_logical_path("C:/outside")); },
            "Windows drive path with slashes was accepted");
        require_throws(
            [] { static_cast<void>(melearner::fixtures::canonical_logical_path("C:outside")); },
            "Windows drive-relative path was accepted");
        require_throws(
            [] { static_cast<void>(melearner::fixtures::canonical_logical_path("\\\\server\\share")); },
            "UNC path was accepted");
        require_throws(
            [] {
                const std::string invalid_utf8 = {static_cast<char>(0xff), 'x'};
                static_cast<void>(melearner::fixtures::canonical_logical_path(invalid_utf8));
            },
            "invalid UTF-8 path was accepted");
        require_throws(
            [] {
                const std::string embedded_nul("Course\0escape", 13);
                static_cast<void>(melearner::fixtures::canonical_logical_path(embedded_nul));
            },
            "embedded NUL path was accepted");
        require_throws(
            [] { static_cast<void>(melearner::fixtures::canonical_logical_path("Course\nLesson")); },
            "control character path was accepted");
        require_throws(
            [&] {
                static_cast<void>(melearner::fixtures::append_logical_path(
                    workspace.root(), "C:\\outside"));
            },
            "physical append accepted a Windows path that can replace its root");
    });

    tests.run("bounded physical smoke keeps the complete logical fixture", [&] {
        smoke = melearner::fixtures::generate_recipe_v1({
            .output_root = smoke_root,
            .physical_mode = PhysicalMode::smoke,
        });
        require_equal(smoke.counts.lessons, 100'000U, "smoke logical Lessons");
        require_equal(smoke.physical_lessons_created, 256U, "smoke created physical Lessons");
        require_equal(smoke.physical_lessons_present, 256U, "smoke present physical Lessons");
        require_equal(smoke.physical_regular_files_present, 256U, "smoke physical files");
        require_equal(regular_file_count(smoke_root / "library"), 256U, "smoke tree files");
        require_equal(regular_file_bytes(smoke_root / "library"), 256U, "placeholder bytes");
        require_contains(
            read_text(smoke_root / "generation-report-v1.json"),
            "\"physicalMode\": \"smoke\"",
            "smoke report mode");
        require(first.expected == smoke.expected, "smoke changed expected logical bytes");
        require(first.scenario == smoke.scenario, "smoke changed scenario logical bytes");
        require(
            first.logical_bundle_sha256 == smoke.logical_bundle_sha256,
            "physical mode changed logical bundle hash");
        require(first.report != smoke.report, "physical reports should record different modes");
    });

    tests.run("workspace refuses to erase a caller-owned nonempty path", [&] {
        const auto guarded = workspace.root() / "guarded-nonempty";
        fs::create_directories(guarded);
        write_text(guarded / "sentinel", "keep\n");
        require_throws(
            [&] {
                Workspace nested{repo_root, guarded};
                static_cast<void>(nested);
            },
            "Workspace accepted a caller-owned nonempty path");
        require(fs::is_regular_file(guarded / "sentinel"), "Workspace erased caller data");
        require_throws(
            [&] {
                Workspace outside{repo_root, repo_root / ".tmp/outside-cpp-parity-owner"};
                static_cast<void>(outside);
            },
            "Workspace accepted a path outside its owned root");
    });

    tests.run("document corpus regenerates byte-for-byte at every boundary", [&] {
        verify_document_contract(
            repo_root / "fixtures/parity/documents",
            workspace.root() / "regenerated-documents");
    });

    tests.run("frozen media hashes and cue/track metadata remain exact", [&] {
        verify_media_contract(repo_root);
    });

    tests.run("manifest v2 inventories corpus, recipes, hashes, and negative cases", [&] {
        verify_manifest_contract(repo_root, workspace.root(), first);
    });

    tests.run("fixture implementation remains test-only and non-installable", [&] {
        verify_source_boundary(
            repo_root,
            read_text(repo_root / "fixtures/parity/fixture-manifest-v2.json"));
    });

    if (run_full) {
        tests.run("full materialization creates exactly 100000 Lesson placeholders", [&] {
            const auto full_root = workspace.root() / "physical-full";
            const auto started = std::chrono::steady_clock::now();
            const auto full = melearner::fixtures::generate_recipe_v1({
                .output_root = full_root,
                .physical_mode = PhysicalMode::full,
            });
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            require_equal(full.physical_lessons_created, 100'000U, "full created physical Lessons");
            require_equal(full.physical_lessons_present, 99'802U, "full present physical Lessons");
            require_equal(
                full.physical_regular_files_present,
                99'805U,
                "full present physical regular files");
            require_equal(
                regular_file_count(full_root / "library"),
                99'805U,
                "full materialized tree files");
            require_equal(
                regular_file_bytes(full_root / "library"),
                full.physical_bytes_present,
                "full materialized tree bytes");
            verify_physical_fingerprint_tree(
                full_root / "library", full_root / "scenario-v1.ndjson");
            require(fs::exists(
                        melearner::fixtures::append_logical_path(
                            full_root / "library",
                            "Systems 日本語/Section 0008/Lesson 000000.txt")),
                    "full tree lacks non-ASCII Lesson path");
            const auto duplicate_a = read_text(
                full_root / "library/Duplicate Marker A/.melearner-course.json");
            const auto duplicate_b = read_text(
                full_root / "library/Duplicate Marker B/.melearner-course.json");
            require(duplicate_a == duplicate_b, "physical duplicate-marker pair is not deliberate");
            require(
                !fs::exists(full_root / "library/Retained Missing A")
                    && !fs::exists(full_root / "library/Retained Missing B"),
                "retained-missing Course paths remain in the final scan tree");
            require_equal(
                fs::file_size(
                    full_root
                    / "library/Ambiguous Lesson Metadata/Section 0007/Ambiguous A.txt"),
                17U,
                "ambiguous Lesson A physical size");
            require_equal(
                fs::file_size(
                    full_root
                    / "library/Ambiguous Lesson Metadata/Section 0007/Ambiguous B.txt"),
                17U,
                "ambiguous Lesson B physical size");
            require(first.expected == full.expected, "full mode changed expected logical bytes");
            require(first.scenario == full.scenario, "full mode changed scenario logical bytes");
            require(
                first.logical_bundle_sha256 == full.logical_bundle_sha256,
                "full mode changed logical bundle hash");
            require(first.report != full.report, "full report should differ from no-physical report");
            const auto total_files = regular_file_count(full_root);
            const auto total_bytes = regular_file_bytes(full_root);
            std::cout << "FULL elapsedMs=" << elapsed.count()
                      << " outputRegularFiles=" << total_files
                      << " physicalRegularFilesPresent=" << full.physical_regular_files_present
                      << " physicalLessonFilesCreated=" << full.physical_lessons_created
                      << " physicalLessonFilesPresent=" << full.physical_lessons_present
                      << " outputBytes=" << total_bytes << '\n';
        });
    }

    std::cout << "SUMMARY expectedBytes=" << first.expected.bytes
              << " expectedSha256=" << first.expected.sha256
              << " scenarioBytes=" << first.scenario.bytes
              << " scenarioSha256=" << first.scenario.sha256
              << " reportBytes=" << first.report.bytes
              << " reportSha256=" << first.report.sha256
              << " bundleSha256=" << first.logical_bundle_sha256
              << " testsPassed=" << tests.passed() << " testsFailed=" << tests.failed() << '\n';
    return tests.result();
}
