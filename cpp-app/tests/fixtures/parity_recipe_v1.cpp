#include "parity_fixture.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace melearner::fixtures {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kRecordBatchLimit = 256;
constexpr std::size_t kCourseCount = 1'000;
constexpr std::size_t kLessonCount = 100'000;
constexpr std::size_t kActivityDateCount = 84;
constexpr std::size_t kNoteCount = 201;

[[nodiscard]] std::string fixed_width(std::size_t value, std::size_t width) {
    std::array<char, 32> digits{};
    const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("cannot format deterministic fixture number");
    }
    const auto length = static_cast<std::size_t>(result.ptr - digits.data());
    if (length > width) {
        throw std::runtime_error("deterministic fixture number exceeds its fixed width");
    }
    std::string output(width - length, '0');
    output.append(digits.data(), result.ptr);
    return output;
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

void write_text_file(const fs::path& path, std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot write generated fixture: " + path.string());
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        throw std::runtime_error("cannot finish generated fixture: " + path.string());
    }
    output.close();
    if (output.fail()) {
        throw std::runtime_error("cannot close generated fixture: " + path.string());
    }
}

class RecordBatchWriter final {
public:
    explicit RecordBatchWriter(const fs::path& path) : output_(path, std::ios::binary | std::ios::trunc) {
        if (!output_) {
            throw std::runtime_error("cannot create generated fixture: " + path.string());
        }
        records_.reserve(kRecordBatchLimit);
    }

    void append(std::string record) {
        if (finished_) {
            throw std::logic_error("cannot append after fixture output is finished");
        }
        records_.push_back(std::move(record));
        peak_ = std::max(peak_, records_.size());
        if (records_.size() == kRecordBatchLimit) {
            flush();
        }
    }

    void finish() {
        if (!finished_) {
            flush();
            output_.flush();
            if (!output_) {
                throw std::runtime_error("cannot finish generated NDJSON fixture");
            }
            output_.close();
            if (output_.fail()) {
                throw std::runtime_error("cannot close generated NDJSON fixture");
            }
            finished_ = true;
        }
    }

    [[nodiscard]] std::size_t peak() const noexcept {
        return peak_;
    }

private:
    void flush() {
        for (const auto& record : records_) {
            output_.write(record.data(), static_cast<std::streamsize>(record.size()));
            output_.put('\n');
        }
        if (!output_) {
            throw std::runtime_error("cannot stream generated NDJSON fixture");
        }
        records_.clear();
    }

    std::ofstream output_;
    std::vector<std::string> records_;
    std::size_t peak_ = 0;
    bool finished_ = false;
};

[[nodiscard]] std::size_t lessons_in_course(std::size_t course_index) {
    if (course_index == 0U) {
        return 1'000U;
    }
    if (course_index < 100U) {
        return 100U;
    }
    return 99U;
}

[[nodiscard]] std::string course_id(std::size_t course_index) {
    return "course-" + fixed_width(course_index, 4);
}

[[nodiscard]] std::string course_name(std::size_t course_index) {
    switch (course_index) {
    case 2:
        return "Duplicate Marker A";
    case 3:
        return "Duplicate Marker B";
    case 4:
        return "Duplicate Fingerprint A";
    case 5:
        return "Duplicate Fingerprint B";
    case 6:
        return "Moved Marker Course";
    case 7:
        return "Ambiguous Lesson Metadata";
    case 8:
        return "Systems 日本語";
    case 9:
        return "Long Logical Path";
    case 998:
        return "Retained Missing A";
    case 999:
        return "Retained Missing B";
    default:
        return "Course " + fixed_width(course_index, 4);
    }
}

[[nodiscard]] std::string course_logical_path(std::size_t course_index) {
    switch (course_index) {
    case 6:
        return "Moved Course";
    case 8:
        return "Systems 日本語";
    case 998:
        return "Retained Missing A";
    case 999:
        return "Retained Missing B";
    default:
        return course_name(course_index);
    }
}

[[nodiscard]] std::size_t fingerprint_basis_index(std::size_t course_index) {
    return course_index == 5U ? 4U : course_index;
}

[[nodiscard]] std::string section_name(std::size_t course_index) {
    return "Section " + fixed_width(fingerprint_basis_index(course_index), 4);
}

[[nodiscard]] std::string fingerprint_basis_id(std::size_t course_index) {
    return "fingerprint-basis-v1-" + fixed_width(fingerprint_basis_index(course_index), 4);
}

[[nodiscard]] std::string course_marker(std::size_t course_index) {
    if (course_index == 2U || course_index == 3U) {
        return "identity-duplicate-marker";
    }
    if (course_index == 4U || course_index == 5U) {
        return {};
    }
    if (course_index == 6U) {
        return "identity-course-0006";
    }
    return "identity-course-" + fixed_width(course_index, 4);
}

struct LessonRecord {
    std::string id;
    std::string logical_path;
    std::string relative_path;
    std::string name;
    std::uint64_t file_bytes = 0;
};

[[nodiscard]] LessonRecord lesson_record(std::size_t course_index, std::size_t lesson_index) {
    const auto number = fixed_width(lesson_index, 6);
    LessonRecord lesson{
        .id = "lesson-" + fixed_width(course_index, 4) + "-" + number,
        .logical_path = {},
        .relative_path = section_name(course_index) + "/Lesson " + number + ".txt",
        .name = "Lesson " + number,
        .file_bytes = 1,
    };
    if (course_index == 7U && lesson_index < 2U) {
        lesson.relative_path = lesson_index == 0U
            ? section_name(course_index) + "/Ambiguous A.txt"
            : section_name(course_index) + "/Ambiguous B.txt";
        lesson.name = "Ambiguous Lesson";
        lesson.file_bytes = 17U;
    } else if (course_index == 9U) {
        lesson.relative_path = section_name(course_index) + "/" + std::string(84, 'a') + "/"
            + std::string(84, 'b') + "/" + std::string(84, 'c') + "/Lesson " + number + ".txt";
    }
    lesson.logical_path = canonical_logical_path(
        course_logical_path(course_index) + "/" + lesson.relative_path);
    return lesson;
}

[[nodiscard]] std::string activity_date(std::size_t index) {
    if (index < 29U) {
        return "2026-06-" + fixed_width(index + 2U, 2);
    }
    if (index < 60U) {
        return "2026-07-" + fixed_width(index - 28U, 2);
    }
    return "2026-08-" + fixed_width(index - 59U, 2);
}

[[nodiscard]] std::string physical_mode_name(PhysicalMode mode) {
    switch (mode) {
    case PhysicalMode::none:
        return "none";
    case PhysicalMode::smoke:
        return "smoke";
    case PhysicalMode::full:
        return "full";
    }
    throw std::logic_error("unknown physical fixture mode");
}

void write_placeholder(
    const fs::path& path,
    std::uint64_t file_bytes,
    fs::path& previous_parent) {
    if (path.parent_path() != previous_parent) {
        fs::create_directories(path.parent_path());
        previous_parent = path.parent_path();
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot materialize Lesson placeholder: " + path.string());
    }
    if (file_bytes == 17U) {
        constexpr std::string_view ambiguous_contents = "ambiguous lesson\n";
        static_assert(ambiguous_contents.size() == 17U);
        output.write(
            ambiguous_contents.data(),
            static_cast<std::streamsize>(ambiguous_contents.size()));
    } else if (file_bytes == 1U) {
        output.put('x');
    } else {
        throw std::logic_error("Lesson placeholder recipe has an unsupported size");
    }
    if (!output) {
        throw std::runtime_error("cannot write Lesson placeholder: " + path.string());
    }
    output.close();
    if (output.fail()) {
        throw std::runtime_error("cannot close Lesson placeholder: " + path.string());
    }
}

[[nodiscard]] std::uint64_t materialize_special_markers(const fs::path& library_root) {
    std::uint64_t bytes = 0;
    for (const auto course_index : {2U, 3U, 6U}) {
        const auto marker = course_marker(course_index);
        const auto contents = "{\n  \"version\": 1,\n  \"identityId\": \"" + marker + "\"\n}\n";
        const auto course_root = append_logical_path(library_root, course_logical_path(course_index));
        write_text_file(course_root / ".melearner-course.json", contents);
        bytes += static_cast<std::uint64_t>(contents.size());
    }
    return bytes;
}

void ensure_clean_output_root(const fs::path& output_root) {
    if (output_root.empty()) {
        throw std::invalid_argument("fixture output root cannot be empty");
    }
    if (fs::exists(output_root)) {
        if (!fs::is_directory(output_root) || !fs::is_empty(output_root)) {
            throw std::invalid_argument("fixture output root must be absent or empty");
        }
    } else {
        fs::create_directories(output_root);
    }
}

}  // namespace

GenerationResult generate_recipe_v1(const GenerationOptions& options) {
    ensure_clean_output_root(options.output_root);
    const auto expected_path = options.output_root / "expected-v1.ndjson";
    const auto scenario_path = options.output_root / "scenario-v1.ndjson";
    const auto report_path = options.output_root / "generation-report-v1.json";
    const auto library_root = options.output_root / "library";

    const auto physical_target = options.physical_mode == PhysicalMode::full
        ? kLessonCount
        : (options.physical_mode == PhysicalMode::smoke ? kRecordBatchLimit : 0U);

    RecordBatchWriter scenario(scenario_path);
    scenario.append(
        "{\"record\":\"fixture\",\"format\":\"melearner-parity-scenario-v1\","
        "\"recipe\":\"cpp-parity-recipe-v1\",\"recordBatchLimit\":256}");

    for (std::size_t course_index = 0; course_index < kCourseCount; ++course_index) {
        const auto marker = course_marker(course_index);
        std::ostringstream record;
        record << "{\"record\":\"course\",\"id\":\"" << course_id(course_index)
               << "\",\"name\":\"" << json_escape(course_name(course_index))
               << "\",\"logicalPath\":\"" << json_escape(course_logical_path(course_index))
               << "\",\"sectionName\":\"" << section_name(course_index)
               << "\",\"fingerprintBasisId\":\"" << fingerprint_basis_id(course_index)
               << "\",\"fingerprintFields\":[\"sectionName\",\"relativePath\","
                  "\"type\",\"fileBytes\"]"
               << ",\"state\":\""
               << (course_index >= 998U ? "retained-missing" : "available")
               << "\",\"markerIdentity\":";
        if (marker.empty()) {
            record << "null";
        } else {
            record << "\"" << marker << "\"";
        }
        record << ",\"lessonCount\":" << lessons_in_course(course_index) << "}";
        scenario.append(record.str());
    }

    std::size_t generated_lessons = 0;
    std::uint64_t physical_lessons_created = 0;
    std::uint64_t physical_bytes_created = 0;
    fs::path previous_physical_parent;
    for (std::size_t course_index = 0; course_index < kCourseCount; ++course_index) {
        const auto count = lessons_in_course(course_index);
        for (std::size_t lesson_index = 0; lesson_index < count; ++lesson_index) {
            const auto lesson = lesson_record(course_index, lesson_index);
            std::ostringstream record;
            record << "{\"record\":\"lesson\",\"id\":\"" << lesson.id
                   << "\",\"courseId\":\"" << course_id(course_index)
                   << "\",\"logicalPath\":\"" << json_escape(lesson.logical_path)
                   << "\",\"relativePath\":\"" << json_escape(lesson.relative_path)
                   << "\",\"name\":\"" << json_escape(lesson.name)
                   << "\",\"type\":\"document\",\"fileBytes\":" << lesson.file_bytes << "}";
            scenario.append(record.str());

            if (generated_lessons < physical_target) {
                write_placeholder(
                    append_logical_path(library_root, lesson.logical_path),
                    lesson.file_bytes,
                    previous_physical_parent);
                ++physical_lessons_created;
                physical_bytes_created += lesson.file_bytes;
            }
            ++generated_lessons;
        }
    }
    if (generated_lessons != kLessonCount) {
        throw std::logic_error("fixture Lesson formula did not produce 100000 records");
    }

    for (std::size_t date_index = 0; date_index < kActivityDateCount; ++date_index) {
        std::ostringstream record;
        record << "{\"record\":\"activityDate\",\"date\":\"" << activity_date(date_index)
               << "\",\"watchedSeconds\":" << ((date_index % 12U) + 1U) * 60U
               << ",\"lessonsTouched\":" << ((date_index % 4U) + 1U)
               << ",\"completions\":" << (date_index % 3U) << "}";
        scenario.append(record.str());
    }

    for (std::size_t note_index = 0; note_index < kNoteCount; ++note_index) {
        std::ostringstream record;
        record << "{\"record\":\"note\",\"id\":\"note-" << fixed_width(note_index, 3)
               << "\",\"lessonId\":\"lesson-0001-000000\",\"timestampSeconds\":42,"
               << "\"createdAt\":\"2026-08-24T12:00:00.000Z\",\"text\":\"Stable note "
               << fixed_width(note_index, 3) << "\"}";
        scenario.append(record.str());
    }

    {
        std::ostringstream record;
        record << "{\"record\":\"identityCase\",\"case\":\"duplicate-marker\","
               << "\"persisted\":["
               << "{\"courseId\":\"course-0002\",\"identityId\":\"identity-course-0002\","
                << "\"logicalPath\":\"Old Marker A\",\"fingerprintBasisId\":\""
               << fingerprint_basis_id(2U) << "\"},"
               << "{\"courseId\":\"course-0003\",\"identityId\":\"identity-course-0003\","
                << "\"logicalPath\":\"Old Marker B\",\"fingerprintBasisId\":\""
               << fingerprint_basis_id(3U) << "\"}],"
               << "\"scanned\":["
                << "{\"scanId\":\"scan-marker-a\",\"logicalPath\":\"Duplicate Marker A\","
               << "\"markerIdentity\":\"identity-duplicate-marker\",\"fingerprintBasisId\":\""
               << fingerprint_basis_id(2U) << "\"},"
                << "{\"scanId\":\"scan-marker-b\",\"logicalPath\":\"Duplicate Marker B\","
               << "\"markerIdentity\":\"identity-duplicate-marker\",\"fingerprintBasisId\":\""
               << fingerprint_basis_id(3U) << "\"}],"
               << "\"outcome\":{\"warning\":\"duplicate-marker\",\"markerMatchesIgnored\":true,"
               << "\"matches\":[{\"scanId\":\"scan-marker-a\",\"courseId\":\"course-0002\","
               << "\"by\":\"fingerprint\"},{\"scanId\":\"scan-marker-b\","
               << "\"courseId\":\"course-0003\",\"by\":\"fingerprint\"}]}}";
        scenario.append(record.str());
    }
    {
        std::ostringstream record;
        record << "{\"record\":\"identityCase\",\"case\":\"duplicate-fingerprint\","
               << "\"persisted\":["
               << "{\"courseId\":\"course-0004\",\"identityId\":\"identity-course-0004\","
                << "\"logicalPath\":\"Duplicate Fingerprint A\",\"fingerprintBasisId\":\""
               << fingerprint_basis_id(4U) << "\"},"
               << "{\"courseId\":\"course-0005\",\"identityId\":\"identity-course-0005\","
                << "\"logicalPath\":\"Duplicate Fingerprint B\",\"fingerprintBasisId\":\""
               << fingerprint_basis_id(5U) << "\"}],"
               << "\"scanned\":{\"scanId\":\"scan-fingerprint-copy\","
                << "\"logicalPath\":\"Another Fingerprint Copy\",\"markerIdentity\":null,"
               << "\"fingerprintBasisId\":\"" << fingerprint_basis_id(4U) << "\"},"
               << "\"outcome\":{\"warning\":\"multiple-existing-courses\","
               << "\"matchedCourseId\":null,\"action\":\"create-new-course\"}}";
        scenario.append(record.str());
    }
    {
        std::ostringstream record;
        record << "{\"record\":\"identityCase\",\"case\":\"moved-marker\","
               << "\"persisted\":{\"courseId\":\"course-0006\","
               << "\"identityId\":\"identity-course-0006\","
                << "\"logicalPath\":\"Original Course\","
               << "\"fingerprintBasisId\":\"fingerprint-basis-v1-old-0006\"},"
               << "\"scanned\":{\"scanId\":\"scan-moved-marker\","
                << "\"logicalPath\":\"Moved Course\","
               << "\"markerIdentity\":\"identity-course-0006\",\"fingerprintBasisId\":\""
               << fingerprint_basis_id(6U) << "\"},"
               << "\"outcome\":{\"matchedCourseId\":\"course-0006\",\"by\":\"marker\"}}";
        scenario.append(record.str());
    }
    {
        std::ostringstream record;
        record << "{\"record\":\"identityCase\",\"case\":\"ambiguous-lesson-metadata\","
               << "\"courseId\":\"course-0007\",\"persisted\":["
               << "{\"lessonId\":\"lesson-0007-000000\","
                << "\"logicalPath\":\"Ambiguous Lesson Metadata/Section 0007/Ambiguous A.txt\","
               << "\"relativePath\":\"Section 0007/Ambiguous A.txt\","
               << "\"name\":\"Ambiguous Lesson\",\"type\":\"document\",\"fileBytes\":17},"
               << "{\"lessonId\":\"lesson-0007-000001\","
                << "\"logicalPath\":\"Ambiguous Lesson Metadata/Section 0007/Ambiguous B.txt\","
               << "\"relativePath\":\"Section 0007/Ambiguous B.txt\","
               << "\"name\":\"Ambiguous Lesson\",\"type\":\"document\",\"fileBytes\":17}],"
               << "\"scanned\":{\"scanId\":\"scan-ambiguous-lesson\","
                << "\"logicalPath\":\"Ambiguous Lesson Metadata/Section 0007/Incoming Ambiguous.txt\","
               << "\"relativePath\":\"Section 0007/Incoming Ambiguous.txt\","
               << "\"name\":\"Ambiguous Lesson\",\"type\":\"document\",\"fileBytes\":17},"
               << "\"outcome\":{\"warning\":\"ambiguous-lesson-metadata\","
               << "\"matchedLessonId\":null,\"action\":\"create-new-lesson\"}}";
        scenario.append(record.str());
    }
    for (const auto course_index : {998U, 999U}) {
        std::ostringstream record;
        record << "{\"record\":\"operation\",\"operation\":\"removeCourse\","
               << "\"courseId\":\"" << course_id(course_index)
               << "\",\"logicalPath\":\"" << course_logical_path(course_index)
               << "\",\"lessonCount\":99,\"expectedState\":\"retained-missing\"}";
        scenario.append(record.str());
    }
    scenario.append(
        "{\"record\":\"pathCase\",\"case\":\"mixed-separators\","
        "\"input\":\"Systems 日本語//Section 0008\\\\Lesson 000000.txt\","
        "\"expectedCanonical\":\"Systems 日本語/Section 0008/Lesson 000000.txt\"}");
    scenario.finish();

    RecordBatchWriter expected(expected_path);
    expected.append(
        "{\"record\":\"summary\",\"courses\":1000,\"lessons\":100000,"
        "\"retainedMissingCourses\":2,\"presentLessonsAfterRemovals\":99802,"
        "\"activityDates\":84,\"notes\":201}");
    expected.append(
        "{\"record\":\"pageShape\",\"projection\":\"courses\",\"pageSize\":128,"
        "\"rows\":[128,128,128,128,128,128,128,104]}");
    expected.append(
        "{\"record\":\"pageShape\",\"projection\":\"largeCourseLessons\","
        "\"courseId\":\"course-0000\",\"pageSize\":256,\"rows\":[256,256,256,232]}");
    expected.append(
        "{\"record\":\"pageShape\",\"projection\":\"notes\",\"lessonId\":"
        "\"lesson-0001-000000\",\"pageSize\":100,\"rows\":[100,100,1],"
        "\"order\":[\"createdAt\",\"id\"]}");
    expected.append(
        "{\"record\":\"identityExpectation\",\"case\":\"duplicate-marker\","
        "\"outcome\":\"warn-ignore-marker-and-match-by-fingerprint\"}");
    expected.append(
        "{\"record\":\"identityExpectation\",\"case\":\"duplicate-fingerprint\","
        "\"outcome\":\"ambiguous-refuse-reuse\"}");
    expected.append(
        "{\"record\":\"identityExpectation\",\"case\":\"moved-marker\","
        "\"outcome\":\"reuse-course-0006\"}");
    expected.append(
        "{\"record\":\"identityExpectation\",\"case\":\"ambiguous-lesson-metadata\","
        "\"outcome\":\"warn-and-create-new\"}");
    expected.append(
        "{\"record\":\"activityWindow\",\"firstDate\":\"2026-06-02\","
        "\"lastDate\":\"2026-08-24\",\"dates\":84,\"timezone\":\"UTC\"}");
    expected.append(
        "{\"record\":\"pathExpectation\",\"cases\":[\"root-relative-posix\","
        "\"non-ascii\",\"logical-path-over-260-bytes\",\"separator-canonicalization\"]}");
    expected.finish();

    std::uint64_t physical_lessons_present = physical_lessons_created;
    std::uint64_t physical_regular_files_present = physical_lessons_created;
    std::uint64_t physical_bytes_present = physical_bytes_created;
    if (options.physical_mode == PhysicalMode::full) {
        physical_bytes_present += materialize_special_markers(library_root);
        physical_regular_files_present += 3U;
        for (const auto course_index : {998U, 999U}) {
            const auto missing_path = append_logical_path(
                library_root, course_logical_path(course_index));
            if (!fs::is_directory(missing_path)) {
                throw std::logic_error("retained-missing Course baseline was not materialized");
            }
            fs::remove_all(missing_path);
            if (fs::exists(missing_path)) {
                throw std::runtime_error("cannot remove retained-missing Course from final scan tree");
            }
            physical_lessons_present -= lessons_in_course(course_index);
            physical_regular_files_present -= lessons_in_course(course_index);
            physical_bytes_present -= lessons_in_course(course_index);
        }
    }

    GenerationResult result{
        .counts = {
            .courses = kCourseCount,
            .lessons = kLessonCount,
            .activity_dates = kActivityDateCount,
            .notes = kNoteCount,
        },
        .peak_buffered_records = std::max(scenario.peak(), expected.peak()),
        .physical_lessons_created = physical_lessons_created,
        .physical_lessons_present = physical_lessons_present,
        .physical_regular_files_present = physical_regular_files_present,
        .physical_bytes_present = physical_bytes_present,
        .expected = digest_file(expected_path),
        .scenario = digest_file(scenario_path),
        .report = {},
        .logical_bundle_sha256 = {},
    };
    result.logical_bundle_sha256 = sha256_text(
        "expected-v1.ndjson\n" + std::to_string(result.expected.bytes) + "\n"
        + result.expected.sha256 + "\nscenario-v1.ndjson\n"
        + std::to_string(result.scenario.bytes) + "\n" + result.scenario.sha256 + "\n");

    std::ostringstream report;
    report << "{\n"
           << "  \"formatVersion\": 1,\n"
           << "  \"recipe\": \"cpp-parity-recipe-v1\",\n"
           << "  \"logicalRoot\": \"library\",\n"
           << "  \"physicalMode\": \"" << physical_mode_name(options.physical_mode) << "\",\n"
           << "  \"recordBatchLimit\": 256,\n"
           << "  \"peakBufferedRecords\": " << result.peak_buffered_records << ",\n"
           << "  \"counts\": {\n"
           << "    \"courses\": 1000,\n"
           << "    \"lessons\": 100000,\n"
           << "    \"retainedMissingCourses\": 2,\n"
           << "    \"activityDates\": 84,\n"
           << "    \"notes\": 201\n"
           << "  },\n"
           << "  \"pageShapes\": {\n"
           << "    \"courses\": [128, 128, 128, 128, 128, 128, 128, 104],\n"
           << "    \"largeCourseLessons\": [256, 256, 256, 232],\n"
           << "    \"notes\": [100, 100, 1]\n"
           << "  },\n"
           << "  \"physical\": {\n"
           << "    \"lessonFilesCreated\": " << result.physical_lessons_created << ",\n"
           << "    \"lessonFilesPresent\": " << result.physical_lessons_present << ",\n"
           << "    \"regularFilesPresent\": " << result.physical_regular_files_present << ",\n"
           << "    \"bytesPresent\": " << result.physical_bytes_present << "\n"
           << "  },\n"
           << "  \"logicalBundleSha256\": \"" << result.logical_bundle_sha256 << "\",\n"
           << "  \"outputs\": {\n"
           << "    \"expected-v1.ndjson\": {\"bytes\": " << result.expected.bytes
           << ", \"sha256\": \"" << result.expected.sha256 << "\"},\n"
           << "    \"scenario-v1.ndjson\": {\"bytes\": " << result.scenario.bytes
           << ", \"sha256\": \"" << result.scenario.sha256 << "\"}\n"
           << "  }\n"
           << "}\n";
    write_text_file(report_path, report.str());
    result.report = digest_file(report_path);
    return result;
}

}  // namespace melearner::fixtures
