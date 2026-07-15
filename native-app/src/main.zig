const std = @import("std");
const builtin = @import("builtin");
const core_adapter = @import("core_adapter.zig");
const native_sdk = @import("native_sdk");
const runner = @import("runner");

pub const panic = std.debug.FullPanic(native_sdk.debug.capturePanic);

const canvas = native_sdk.canvas;
const geometry = native_sdk.geometry;

pub const canvas_label = "library-canvas";
pub const window_width: f32 = 960;
pub const window_height: f32 = 680;
pub const window_min_width: f32 = 560;
pub const window_min_height: f32 = 400;
pub const content_max_width: f32 = 960;
pub const course_split_breakpoint: f32 = 768;
const max_courses: usize = core_adapter.library_page_size;
const max_lessons: usize = core_adapter.lesson_page_size;
const max_search_results: usize = core_adapter.search_page_size;
const max_course_id_bytes = core_adapter.max_course_id_bytes;
const max_course_name_bytes = 512;
const max_lesson_id_bytes = 128;
const max_lesson_name_bytes = 512;
const max_section_id_bytes = 128;
const max_section_name_bytes = 512;
const max_lesson_kind_bytes = 16;
const max_search_result_key_bytes = "lesson/".len + max_lesson_id_bytes;
const max_library_message_bytes = 256;
const max_course_message_bytes = 256;
const max_search_message_bytes = 256;

const shell_views = [_]native_sdk.ShellView{
    .{ .label = canvas_label, .kind = .gpu_surface, .fill = true, .role = "Library canvas", .accessibility_label = "melearner Library", .gpu_backend = .metal, .gpu_pixel_format = .bgra8_unorm, .gpu_present_mode = .timer, .gpu_alpha_mode = .@"opaque", .gpu_color_space = .srgb, .gpu_vsync = true },
};
const shell_windows = [_]native_sdk.ShellWindow{.{
    .label = "main",
    .title = "melearner",
    .width = window_width,
    .height = window_height,
    .min_width = window_min_width,
    .min_height = window_min_height,
    .restore_state = false,
    .views = &shell_views,
}};
const shell_scene: native_sdk.ShellConfig = .{ .windows = &shell_windows };

pub const Course = struct {
    id_storage: [max_course_id_bytes]u8 = [_]u8{0} ** max_course_id_bytes,
    id_len: usize = 0,
    name_storage: [max_course_name_bytes]u8 = [_]u8{0} ** max_course_name_bytes,
    name_len: usize = 0,
    lesson_count: u64 = 0,
    completed_lesson_count: u64 = 0,
    progress_percent: u32 = 0,
    available: bool = true,
    selected: bool = false,
    restore_focus: bool = false,

    pub fn id(course: *const Course) []const u8 {
        return course.id_storage[0..course.id_len];
    }

    pub fn name(course: *const Course) []const u8 {
        return course.name_storage[0..course.name_len];
    }

    pub fn progressLine(course: *const Course, arena: std.mem.Allocator) []const u8 {
        return std.fmt.allocPrint(arena, "{d} of {d} Lessons \u{b7} {d}%", .{
            course.completed_lesson_count,
            course.lesson_count,
            course.progress_percent,
        }) catch "";
    }

    pub fn progressValue(course: *const Course) f32 {
        return @as(f32, @floatFromInt(course.progress_percent)) / 100;
    }

    pub fn progressLabel(course: *const Course, arena: std.mem.Allocator) []const u8 {
        return std.fmt.allocPrint(arena, "{s} Progress", .{course.name()}) catch "Course Progress";
    }
};

fn lessonKindLabel(kind: []const u8) []const u8 {
    return if (std.mem.eql(u8, kind, "video"))
        "Video"
    else if (std.mem.eql(u8, kind, "audio"))
        "Audio"
    else if (std.mem.eql(u8, kind, "quiz"))
        "Quiz"
    else
        "Document";
}

pub const Lesson = struct {
    id_storage: [max_lesson_id_bytes]u8 = [_]u8{0} ** max_lesson_id_bytes,
    id_len: usize = 0,
    section_id_storage: [max_section_id_bytes]u8 = [_]u8{0} ** max_section_id_bytes,
    section_id_len: usize = 0,
    section_name_storage: [max_section_name_bytes]u8 = [_]u8{0} ** max_section_name_bytes,
    section_name_len: usize = 0,
    name_storage: [max_lesson_name_bytes]u8 = [_]u8{0} ** max_lesson_name_bytes,
    name_len: usize = 0,
    kind_storage: [max_lesson_kind_bytes]u8 = [_]u8{0} ** max_lesson_kind_bytes,
    kind_len: usize = 0,
    duration: u64 = 0,
    watched_time: u64 = 0,
    last_position: f64 = 0,
    completed: bool = false,
    starts_section: bool = false,
    section_expanded: bool = true,
    selected: bool = false,
    restore_focus: bool = false,

    pub fn id(lesson: *const Lesson) []const u8 {
        return lesson.id_storage[0..lesson.id_len];
    }

    pub fn sectionId(lesson: *const Lesson) []const u8 {
        return lesson.section_id_storage[0..lesson.section_id_len];
    }

    pub fn sectionName(lesson: *const Lesson) []const u8 {
        return lesson.section_name_storage[0..lesson.section_name_len];
    }

    pub fn name(lesson: *const Lesson) []const u8 {
        return lesson.name_storage[0..lesson.name_len];
    }

    pub fn kindLabel(lesson: *const Lesson) []const u8 {
        return lessonKindLabel(lesson.kind_storage[0..lesson.kind_len]);
    }

    pub fn sectionToggleLabel(lesson: *const Lesson) []const u8 {
        return if (lesson.section_expanded) "Collapse" else "Expand";
    }

    pub fn progressLine(lesson: *const Lesson, arena: std.mem.Allocator) []const u8 {
        if (lesson.completed) return "Completed";
        if (lesson.watched_time != 0) {
            return std.fmt.allocPrint(arena, "{d} min watched", .{(lesson.watched_time + 59) / 60}) catch "";
        }
        if (lesson.duration != 0) {
            return std.fmt.allocPrint(arena, "{d} min", .{(lesson.duration + 59) / 60}) catch "";
        }
        return "Not started";
    }
};

pub const SearchResultType = enum {
    course,
    lesson,
};

pub const SearchResult = struct {
    result_type: SearchResultType = .lesson,
    action_key_storage: [max_search_result_key_bytes]u8 = [_]u8{0} ** max_search_result_key_bytes,
    action_key_len: usize = 0,
    id_storage: [max_lesson_id_bytes]u8 = [_]u8{0} ** max_lesson_id_bytes,
    id_len: usize = 0,
    course_id_storage: [max_course_id_bytes]u8 = [_]u8{0} ** max_course_id_bytes,
    course_id_len: usize = 0,
    course_name_storage: [max_course_name_bytes]u8 = [_]u8{0} ** max_course_name_bytes,
    course_name_len: usize = 0,
    section_name_storage: [max_section_name_bytes]u8 = [_]u8{0} ** max_section_name_bytes,
    section_name_len: usize = 0,
    name_storage: [max_lesson_name_bytes]u8 = [_]u8{0} ** max_lesson_name_bytes,
    name_len: usize = 0,
    kind_storage: [max_lesson_kind_bytes]u8 = [_]u8{0} ** max_lesson_kind_bytes,
    kind_len: usize = 0,
    lesson_offset: u64 = 0,

    pub fn id(result: *const SearchResult) []const u8 {
        return result.id_storage[0..result.id_len];
    }

    pub fn courseId(result: *const SearchResult) []const u8 {
        return result.course_id_storage[0..result.course_id_len];
    }

    pub fn courseName(result: *const SearchResult) []const u8 {
        return result.course_name_storage[0..result.course_name_len];
    }

    pub fn sectionName(result: *const SearchResult) []const u8 {
        return result.section_name_storage[0..result.section_name_len];
    }

    pub fn name(result: *const SearchResult) []const u8 {
        return result.name_storage[0..result.name_len];
    }

    pub fn actionKey(result: *const SearchResult) []const u8 {
        return result.action_key_storage[0..result.action_key_len];
    }

    pub fn kindLabel(result: *const SearchResult) []const u8 {
        if (result.result_type == .course) return "Course";
        return lessonKindLabel(result.kind_storage[0..result.kind_len]);
    }

    pub fn contextLine(result: *const SearchResult, arena: std.mem.Allocator) []const u8 {
        if (result.result_type == .course) return "Course outline";
        return std.fmt.allocPrint(arena, "{s} · {s}", .{ result.courseName(), result.sectionName() }) catch "";
    }
};

pub const LibraryState = enum {
    opening,
    empty,
    ready,
    failed,
};

pub const Screen = enum {
    library,
    course,
};

pub const CourseState = enum {
    inactive,
    accessing,
    loading,
    empty,
    ready,
    failed,
};

pub const CompactCoursePage = enum {
    outline,
    lesson,
};

pub const SearchState = enum {
    inactive,
    building,
    ready,
    querying,
    empty,
    results,
    failed,
};

pub const Model = struct {
    appearance: native_sdk.Appearance = .{},
    canvas_width: f32 = window_width,
    library_state: LibraryState = .opening,
    library_revision: u64 = 0,
    total_courses: u64 = 0,
    page_offset: u64 = 0,
    pending_page_offset: u64 = 0,
    library_request_id: u64 = 0,
    course_access_request_id: u64 = 0,
    lesson_page_request_id: u64 = 0,
    courses: [max_courses]Course = undefined,
    course_count: usize = 0,
    library_message_storage: [max_library_message_bytes]u8 = [_]u8{0} ** max_library_message_bytes,
    library_message_len: usize = 0,
    screen: Screen = .library,
    restore_course_focus: bool = false,
    course_state: CourseState = .inactive,
    compact_course_page: CompactCoursePage = .outline,
    course_split_fraction: f32 = 0.36,
    selected_course: Course = .{},
    total_lessons: u64 = 0,
    lesson_page_offset: u64 = 0,
    pending_lesson_page_offset: u64 = 0,
    lessons: [max_lessons]Lesson = undefined,
    lesson_count: usize = 0,
    selected_lesson: Lesson = .{},
    has_selected_lesson: bool = false,
    selected_lesson_offset: u64 = 0,
    target_lesson_id_storage: [max_lesson_id_bytes]u8 = [_]u8{0} ** max_lesson_id_bytes,
    target_lesson_id_len: usize = 0,
    target_lesson_offset: u64 = 0,
    course_message_storage: [max_course_message_bytes]u8 = [_]u8{0} ** max_course_message_bytes,
    course_message_len: usize = 0,
    search_open: bool = false,
    restore_search_focus: bool = false,
    search_state: SearchState = .inactive,
    search_query_buffer: canvas.TextBuffer(core_adapter.max_search_query_bytes) = .{},
    search_index_revision: u64 = 0,
    search_index_request_id: u64 = 0,
    search_query_request_id: u64 = 0,
    search_query_key: u64 = 0,
    next_search_query_id: u64 = 1,
    desired_search_query_id: u64 = 0,
    active_search_query_id: u64 = 0,
    pending_search_offset: u64 = 0,
    search_offset: u64 = 0,
    total_search_results: u64 = 0,
    search_results: [max_search_results]SearchResult = undefined,
    search_result_count: usize = 0,
    search_message_storage: [max_search_message_bytes]u8 = [_]u8{0} ** max_search_message_bytes,
    search_message_len: usize = 0,

    pub const view_unbound = .{
        "appearance",
        "canvas_width",
        "library_state",
        "library_revision",
        "total_courses",
        "page_offset",
        "pending_page_offset",
        "library_request_id",
        "course_access_request_id",
        "lesson_page_request_id",
        "courses",
        "course_count",
        "library_message_storage",
        "library_message_len",
        "screen",
        "restore_course_focus",
        "course_state",
        "compact_course_page",
        "course_split_fraction",
        "selected_course",
        "total_lessons",
        "lesson_page_offset",
        "pending_lesson_page_offset",
        "lessons",
        "lesson_count",
        "selected_lesson",
        "has_selected_lesson",
        "selected_lesson_offset",
        "target_lesson_id_storage",
        "target_lesson_id_len",
        "target_lesson_offset",
        "course_message_storage",
        "course_message_len",
        "search_open",
        "restore_search_focus",
        "search_state",
        "search_query_buffer",
        "search_index_revision",
        "search_index_request_id",
        "search_query_request_id",
        "search_query_key",
        "next_search_query_id",
        "desired_search_query_id",
        "active_search_query_id",
        "pending_search_offset",
        "search_offset",
        "total_search_results",
        "search_results",
        "search_result_count",
        "search_message_storage",
        "search_message_len",
        "libraryReady",
        "courseReady",
        "showCourse",
    };

    pub fn showLibrary(model: *const Model) bool {
        return model.screen == .library;
    }

    pub fn showCourse(model: *const Model) bool {
        return model.screen == .course;
    }

    pub fn showWideCourse(model: *const Model) bool {
        return model.screen == .course and model.canvas_width >= course_split_breakpoint;
    }

    pub fn showCompactLesson(model: *const Model) bool {
        return model.screen == .course and
            model.canvas_width < course_split_breakpoint and
            model.compact_course_page == .lesson;
    }

    pub fn libraryOpening(model: *const Model) bool {
        return model.library_state == .opening;
    }

    pub fn libraryEmpty(model: *const Model) bool {
        return model.library_state == .empty;
    }

    pub fn libraryReady(model: *const Model) bool {
        return model.library_state == .ready;
    }

    pub fn libraryFailed(model: *const Model) bool {
        return model.library_state == .failed;
    }

    pub fn hasPreviousPage(model: *const Model) bool {
        return model.library_state == .ready and model.page_offset != 0;
    }

    pub fn hasNextPage(model: *const Model) bool {
        return model.library_state == .ready and
            model.page_offset + @as(u64, @intCast(model.course_count)) < model.total_courses;
    }

    pub fn hasPagination(model: *const Model) bool {
        return model.hasPreviousPage() or model.hasNextPage();
    }

    pub fn libraryMessage(model: *const Model) []const u8 {
        return model.library_message_storage[0..model.library_message_len];
    }

    pub fn courseOpening(model: *const Model) bool {
        return model.course_state == .accessing or model.course_state == .loading;
    }

    pub fn courseEmpty(model: *const Model) bool {
        return model.course_state == .empty;
    }

    pub fn courseReady(model: *const Model) bool {
        return model.course_state == .ready;
    }

    pub fn courseFailed(model: *const Model) bool {
        return model.course_state == .failed;
    }

    pub fn selectedCourseName(model: *const Model) []const u8 {
        return model.selected_course.name();
    }

    pub fn selectedCourseProgress(model: *const Model, arena: std.mem.Allocator) []const u8 {
        return model.selected_course.progressLine(arena);
    }

    pub fn selectedLessonName(model: *const Model) []const u8 {
        return if (model.has_selected_lesson) model.selected_lesson.name() else "Select a Lesson";
    }

    pub fn selectedLessonSection(model: *const Model) []const u8 {
        return if (model.has_selected_lesson) model.selected_lesson.sectionName() else "Course outline";
    }

    pub fn selectedLessonKind(model: *const Model) []const u8 {
        return if (model.has_selected_lesson) model.selected_lesson.kindLabel() else "Lesson";
    }

    pub fn selectedLessonProgress(model: *const Model, arena: std.mem.Allocator) []const u8 {
        return if (model.has_selected_lesson) model.selected_lesson.progressLine(arena) else "Choose a Lesson from the outline.";
    }

    pub fn courseMessage(model: *const Model) []const u8 {
        return model.course_message_storage[0..model.course_message_len];
    }

    pub fn courseRows(model: *const Model, arena: std.mem.Allocator) []const Course {
        _ = arena;
        return model.courses[0..model.course_count];
    }

    pub fn lessonRows(model: *const Model, arena: std.mem.Allocator) []const Lesson {
        _ = arena;
        return model.lessons[0..model.lesson_count];
    }

    pub fn searchQuery(model: *const Model) []const u8 {
        return model.search_query_buffer.text();
    }

    pub fn searchRows(model: *const Model, arena: std.mem.Allocator) []const SearchResult {
        _ = arena;
        return model.search_results[0..model.search_result_count];
    }

    pub fn searchBuilding(model: *const Model) bool {
        return model.search_state == .building;
    }

    pub fn searchQuerying(model: *const Model) bool {
        return model.search_state == .querying;
    }

    pub fn searchEmpty(model: *const Model) bool {
        return model.search_state == .empty;
    }

    pub fn searchResultsReady(model: *const Model) bool {
        return model.search_state == .results;
    }

    pub fn searchFailed(model: *const Model) bool {
        return model.search_state == .failed;
    }

    pub fn searchMessage(model: *const Model) []const u8 {
        return model.search_message_storage[0..model.search_message_len];
    }

    pub fn hasPreviousSearchPage(model: *const Model) bool {
        return model.search_state == .results and model.search_offset != 0;
    }

    pub fn hasNextSearchPage(model: *const Model) bool {
        return model.search_state == .results and
            model.search_offset + @as(u64, @intCast(model.search_result_count)) < model.total_search_results;
    }

    pub fn hasSearchPagination(model: *const Model) bool {
        return model.hasPreviousSearchPage() or model.hasNextSearchPage();
    }

    pub fn searchTotalLabel(model: *const Model, arena: std.mem.Allocator) []const u8 {
        if (model.total_search_results == 0) return "No results";
        if (model.total_search_results == 1) return "1 result";
        if (model.search_offset == 0 and
            @as(u64, @intCast(model.search_result_count)) == model.total_search_results)
        {
            return std.fmt.allocPrint(arena, "{d} results", .{model.total_search_results}) catch "";
        }
        const first = model.search_offset + 1;
        const last = model.search_offset + @as(u64, @intCast(model.search_result_count));
        if (first == last) {
            return std.fmt.allocPrint(arena, "{d} of {d} results", .{ first, model.total_search_results }) catch "";
        }
        return std.fmt.allocPrint(arena, "{d}–{d} of {d} results", .{ first, last, model.total_search_results }) catch "";
    }

    fn targetLessonId(model: *const Model) []const u8 {
        return model.target_lesson_id_storage[0..model.target_lesson_id_len];
    }

    fn setTargetLesson(model: *Model, lesson_id: []const u8, lesson_offset: u64) void {
        @memcpy(model.target_lesson_id_storage[0..lesson_id.len], lesson_id);
        model.target_lesson_id_len = lesson_id.len;
        model.target_lesson_offset = lesson_offset;
    }

    fn clearTargetLesson(model: *Model) void {
        model.target_lesson_id_len = 0;
        model.target_lesson_offset = 0;
    }

    fn clearSearchResults(model: *Model) void {
        model.search_offset = 0;
        model.pending_search_offset = 0;
        model.total_search_results = 0;
        model.search_result_count = 0;
        model.search_message_len = 0;
    }

    fn setSearchFailure(model: *Model, message: []const u8) void {
        const value = if (message.len == 0 or message.len > max_search_message_bytes or !std.unicode.utf8ValidateSlice(message))
            "Search is unavailable."
        else
            message;
        @memcpy(model.search_message_storage[0..value.len], value);
        model.search_message_len = value.len;
        model.search_state = .failed;
        model.search_index_request_id = 0;
        model.search_query_request_id = 0;
        model.search_query_key = 0;
        model.active_search_query_id = 0;
        model.search_result_count = 0;
        model.total_search_results = 0;
    }

    pub fn courseTotalLabel(model: *const Model, arena: std.mem.Allocator) []const u8 {
        if (model.page_offset != 0 or model.course_count < model.total_courses) {
            const first = model.page_offset + 1;
            const last = model.page_offset + @as(u64, @intCast(model.course_count));
            if (first == last) {
                return std.fmt.allocPrint(arena, "{d} of {d} Courses", .{
                    first,
                    model.total_courses,
                }) catch "";
            }
            return std.fmt.allocPrint(arena, "{d}–{d} of {d} Courses", .{
                first,
                last,
                model.total_courses,
            }) catch "";
        }
        return std.fmt.allocPrint(arena, "{d} {s}", .{
            model.total_courses,
            if (model.total_courses == 1) "Course" else "Courses",
        }) catch "";
    }

    pub fn hasPreviousLessonPage(model: *const Model) bool {
        return model.course_state == .ready and model.lesson_page_offset != 0;
    }

    pub fn hasNextLessonPage(model: *const Model) bool {
        return model.course_state == .ready and
            model.lesson_page_offset + @as(u64, @intCast(model.lesson_count)) < model.total_lessons;
    }

    pub fn hasLessonPagination(model: *const Model) bool {
        return model.hasPreviousLessonPage() or model.hasNextLessonPage();
    }

    pub fn lessonTotalLabel(model: *const Model, arena: std.mem.Allocator) []const u8 {
        if (model.lesson_page_offset != 0 or model.lesson_count < model.total_lessons) {
            const first = model.lesson_page_offset + 1;
            const last = model.lesson_page_offset + @as(u64, @intCast(model.lesson_count));
            if (first == last) {
                return std.fmt.allocPrint(arena, "{d} of {d} Lessons", .{ first, model.total_lessons }) catch "";
            }
            return std.fmt.allocPrint(arena, "{d}–{d} of {d} Lessons", .{ first, last, model.total_lessons }) catch "";
        }
        return std.fmt.allocPrint(arena, "{d} {s}", .{
            model.total_lessons,
            if (model.total_lessons == 1) "Lesson" else "Lessons",
        }) catch "";
    }

    fn setFailure(model: *Model, message: []const u8) void {
        const value = if (message.len == 0 or message.len > max_library_message_bytes or !std.unicode.utf8ValidateSlice(message))
            "The Library service could not open."
        else
            message;
        @memcpy(model.library_message_storage[0..value.len], value);
        model.library_message_len = value.len;
        model.library_state = .failed;
        model.library_revision = 0;
        model.total_courses = 0;
        model.page_offset = 0;
        model.pending_page_offset = 0;
        model.library_request_id = 0;
        model.course_count = 0;
    }

    fn setCourseFailure(model: *Model, message: []const u8) void {
        const value = if (message.len == 0 or message.len > max_course_message_bytes or !std.unicode.utf8ValidateSlice(message))
            "The Course could not open."
        else
            message;
        @memcpy(model.course_message_storage[0..value.len], value);
        model.course_message_len = value.len;
        model.course_state = .failed;
        model.total_lessons = 0;
        model.lesson_page_offset = 0;
        model.pending_lesson_page_offset = 0;
        model.course_access_request_id = 0;
        model.lesson_page_request_id = 0;
        model.lesson_count = 0;
    }

    fn loadCoursePage(model: *Model, bytes: []const u8) !void {
        const CoursePayload = struct {
            id: []const u8,
            name: []const u8,
            missingSince: ?[]const u8,
            lessonCount: u64,
            completedLessonCount: u64,
            progressPercent: u32,
        };
        const PagePayload = struct {
            revision: u64,
            offset: u64,
            total: u64,
            rows: []const CoursePayload,
        };

        const parsed = try std.json.parseFromSlice(PagePayload, std.heap.page_allocator, bytes, .{
            .ignore_unknown_fields = true,
        });
        defer parsed.deinit();
        const page = parsed.value;
        if (page.revision == 0 or
            (model.library_revision != 0 and page.revision != model.library_revision) or
            page.offset != model.pending_page_offset or
            page.offset % core_adapter.library_page_size != 0 or
            page.offset > page.total or
            page.rows.len > max_courses)
        {
            return error.InvalidLibraryPage;
        }
        const remaining = page.total - page.offset;
        const expected_rows = @min(@as(u64, max_courses), remaining);
        if (page.rows.len != expected_rows or (page.total != 0 and page.offset == page.total)) {
            return error.InvalidLibraryPage;
        }

        var courses: [max_courses]Course = undefined;
        for (page.rows, 0..) |source, index| {
            if (source.id.len == 0 or source.id.len > max_course_id_bytes or
                source.name.len == 0 or source.name.len > max_course_name_bytes or
                (source.missingSince != null and source.missingSince.?.len > 64) or
                source.completedLessonCount > source.lessonCount or source.progressPercent > 100)
            {
                return error.InvalidLibraryPage;
            }
            for (courses[0..index]) |course| {
                if (std.mem.eql(u8, course.id(), source.id)) return error.InvalidLibraryPage;
            }
            courses[index] = .{
                .id_len = source.id.len,
                .name_len = source.name.len,
                .lesson_count = source.lessonCount,
                .completed_lesson_count = source.completedLessonCount,
                .progress_percent = source.progressPercent,
                .available = source.missingSince == null,
                .selected = model.selected_course.id_len != 0 and
                    std.mem.eql(u8, model.selected_course.id(), source.id),
                .restore_focus = model.restore_course_focus and
                    model.selected_course.id_len != 0 and
                    std.mem.eql(u8, model.selected_course.id(), source.id),
            };
            @memcpy(courses[index].id_storage[0..source.id.len], source.id);
            @memcpy(courses[index].name_storage[0..source.name.len], source.name);
        }

        @memcpy(model.courses[0..page.rows.len], courses[0..page.rows.len]);
        model.course_count = page.rows.len;
        model.library_revision = page.revision;
        model.total_courses = page.total;
        model.page_offset = page.offset;
        model.pending_page_offset = page.offset;
        model.library_message_len = 0;
        model.library_state = if (page.total == 0) .empty else .ready;
        model.restore_course_focus = false;
    }

    fn loadCourseAccess(model: *Model, bytes: []const u8) !void {
        const AccessPayload = struct {
            revision: u64,
            courseId: []const u8,
            courseName: []const u8,
            lessonCount: u64,
            completedLessonCount: u64,
            progressPercent: u32,
            resumeLessonId: ?[]const u8,
            resumeLessonOffset: ?u64,
            lastAccessed: []const u8,
        };
        const parsed = try std.json.parseFromSlice(AccessPayload, std.heap.page_allocator, bytes, .{
            .ignore_unknown_fields = true,
        });
        defer parsed.deinit();
        const access = parsed.value;
        const has_resume = access.resumeLessonId != null and access.resumeLessonOffset != null;
        if (model.course_state != .accessing or
            access.revision <= model.library_revision or
            !std.mem.eql(u8, access.courseId, model.selected_course.id()) or
            !validRequiredText(access.courseName, max_course_name_bytes) or
            access.completedLessonCount > access.lessonCount or
            access.progressPercent > 100 or
            has_resume != (access.lessonCount != 0) or
            has_resume != (access.resumeLessonId != null) or
            has_resume != (access.resumeLessonOffset != null) or
            (has_resume and (!validRequiredText(access.resumeLessonId.?, max_lesson_id_bytes) or
                access.resumeLessonOffset.? >= access.lessonCount)) or
            access.lastAccessed.len == 0 or
            access.lastAccessed.len > 64 or
            !std.mem.endsWith(u8, access.lastAccessed, "Z"))
        {
            return error.InvalidCourseAccess;
        }
        model.library_revision = access.revision;
        model.selected_course.name_len = access.courseName.len;
        @memcpy(model.selected_course.name_storage[0..access.courseName.len], access.courseName);
        model.selected_course.lesson_count = access.lessonCount;
        model.selected_course.completed_lesson_count = access.completedLessonCount;
        model.selected_course.progress_percent = access.progressPercent;
        if (model.target_lesson_id_len == 0) {
            if (model.has_selected_lesson) {
                model.setTargetLesson(model.selected_lesson.id(), model.selected_lesson_offset);
            } else if (has_resume) {
                model.setTargetLesson(access.resumeLessonId.?, access.resumeLessonOffset.?);
            }
        }
    }

    fn loadLessonPage(model: *Model, bytes: []const u8) !void {
        const LessonPayload = struct {
            id: []const u8,
            courseId: []const u8,
            sectionId: []const u8,
            sectionName: []const u8,
            name: []const u8,
            kind: []const u8,
            duration: i64,
            fileSize: i64,
            completed: bool,
            watchedTime: i64,
            lastPosition: f64,
            order: i64,
        };
        const PagePayload = struct {
            revision: u64,
            courseId: []const u8,
            sectionId: ?[]const u8,
            offset: u64,
            total: u64,
            rows: []const LessonPayload,
        };

        const parsed = try std.json.parseFromSlice(PagePayload, std.heap.page_allocator, bytes, .{
            .ignore_unknown_fields = true,
        });
        defer parsed.deinit();
        const page = parsed.value;
        if (model.course_state != .loading or
            page.revision != model.library_revision or
            !std.mem.eql(u8, page.courseId, model.selected_course.id()) or
            page.sectionId != null or
            page.offset != model.pending_lesson_page_offset or
            page.offset % core_adapter.lesson_page_size != 0 or
            page.offset > page.total or
            page.rows.len > max_lessons)
        {
            return error.InvalidLessonPage;
        }
        const remaining = page.total - page.offset;
        const expected_rows = @min(@as(u64, max_lessons), remaining);
        if (page.rows.len != expected_rows or (page.total != 0 and page.offset == page.total)) {
            return error.InvalidLessonPage;
        }

        var lessons: [max_lessons]Lesson = undefined;
        for (page.rows, 0..) |source, index| {
            if (!validRequiredText(source.id, max_lesson_id_bytes) or
                !std.mem.eql(u8, source.courseId, model.selected_course.id()) or
                !validRequiredText(source.sectionId, max_section_id_bytes) or
                !validRequiredText(source.sectionName, max_section_name_bytes) or
                !validRequiredText(source.name, max_lesson_name_bytes) or
                !validLessonKind(source.kind) or
                source.duration < 0 or source.fileSize < 0 or source.watchedTime < 0 or source.order < 0 or
                !std.math.isFinite(source.lastPosition) or source.lastPosition < 0)
            {
                return error.InvalidLessonPage;
            }
            for (lessons[0..index]) |lesson| {
                if (std.mem.eql(u8, lesson.id(), source.id)) return error.InvalidLessonPage;
            }
            lessons[index] = .{
                .id_len = source.id.len,
                .section_id_len = source.sectionId.len,
                .section_name_len = source.sectionName.len,
                .name_len = source.name.len,
                .kind_len = source.kind.len,
                .duration = @intCast(source.duration),
                .watched_time = @intCast(source.watchedTime),
                .last_position = source.lastPosition,
                .completed = source.completed,
                .starts_section = index == 0 or !std.mem.eql(u8, source.sectionId, page.rows[index - 1].sectionId),
            };
            @memcpy(lessons[index].id_storage[0..source.id.len], source.id);
            @memcpy(lessons[index].section_id_storage[0..source.sectionId.len], source.sectionId);
            @memcpy(lessons[index].section_name_storage[0..source.sectionName.len], source.sectionName);
            @memcpy(lessons[index].name_storage[0..source.name.len], source.name);
            @memcpy(lessons[index].kind_storage[0..source.kind.len], source.kind);
        }

        var target_index: ?usize = null;
        if (model.target_lesson_id_len != 0) {
            if (model.target_lesson_offset < page.offset or
                model.target_lesson_offset >= page.offset + @as(u64, @intCast(page.rows.len)))
            {
                return error.InvalidLessonPage;
            }
            for (lessons[0..page.rows.len], 0..) |*lesson, index| {
                if (std.mem.eql(u8, lesson.id(), model.targetLessonId())) {
                    target_index = index;
                    break;
                }
            }
            if (target_index == null or
                page.offset + @as(u64, @intCast(target_index.?)) != model.target_lesson_offset)
            {
                return error.InvalidLessonPage;
            }
            lessons[target_index.?].selected = true;
        } else if (model.has_selected_lesson) {
            for (lessons[0..page.rows.len]) |*lesson| {
                if (std.mem.eql(u8, lesson.id(), model.selected_lesson.id())) {
                    lesson.selected = true;
                    break;
                }
            }
        }

        @memcpy(model.lessons[0..page.rows.len], lessons[0..page.rows.len]);
        model.lesson_count = page.rows.len;
        model.total_lessons = page.total;
        model.lesson_page_offset = page.offset;
        model.pending_lesson_page_offset = page.offset;
        model.course_message_len = 0;
        model.course_state = if (page.total == 0) .empty else .ready;
        if (target_index) |index| {
            model.selected_lesson = lessons[index];
            model.has_selected_lesson = true;
            model.selected_lesson_offset = page.offset + @as(u64, @intCast(index));
            model.clearTargetLesson();
        } else if (model.has_selected_lesson) {
            for (lessons[0..page.rows.len], 0..) |lesson, index| {
                if (std.mem.eql(u8, lesson.id(), model.selected_lesson.id())) {
                    model.selected_lesson = lesson;
                    model.selected_lesson_offset = page.offset + @as(u64, @intCast(index));
                    break;
                }
            }
        }
    }

    fn loadSearchIndex(model: *Model, bytes: []const u8) !void {
        const ReadyPayload = struct {
            indexRevision: u64,
            entryCount: u64,
        };
        const parsed = try std.json.parseFromSlice(ReadyPayload, std.heap.page_allocator, bytes, .{
            .ignore_unknown_fields = true,
        });
        defer parsed.deinit();
        if (parsed.value.indexRevision == 0 or parsed.value.indexRevision != model.library_revision) {
            return error.InvalidSearchIndex;
        }
        model.search_index_revision = parsed.value.indexRevision;
        model.search_message_len = 0;
        model.search_state = .ready;
    }

    fn loadSearchPage(model: *Model, bytes: []const u8) !void {
        const SearchHitPayload = struct {
            resultType: []const u8,
            id: []const u8,
            courseId: []const u8,
            courseName: []const u8,
            sectionId: []const u8,
            sectionName: []const u8,
            lessonOffset: u64,
            name: []const u8,
            kind: []const u8,
            score: i32,
        };
        const SearchPagePayload = struct {
            queryId: u64,
            indexRevision: u64,
            offset: u64,
            total: u64,
            rows: []const SearchHitPayload,
        };
        const parsed = try std.json.parseFromSlice(SearchPagePayload, std.heap.page_allocator, bytes, .{
            .ignore_unknown_fields = true,
        });
        defer parsed.deinit();
        const page = parsed.value;
        if (!model.search_open or
            page.queryId != model.active_search_query_id or
            page.queryId != model.desired_search_query_id or
            page.indexRevision != model.search_index_revision or
            page.offset != model.pending_search_offset or
            page.offset % core_adapter.search_page_size != 0 or
            page.offset > page.total or
            page.rows.len > max_search_results)
        {
            return error.InvalidSearchPage;
        }
        const remaining = page.total - page.offset;
        const expected_rows = @min(@as(u64, max_search_results), remaining);
        if (page.rows.len != expected_rows or (page.total != 0 and page.offset == page.total)) {
            return error.InvalidSearchPage;
        }

        var results: [max_search_results]SearchResult = undefined;
        for (page.rows, 0..) |source, index| {
            const result_type: SearchResultType = if (std.mem.eql(u8, source.resultType, "course"))
                .course
            else if (std.mem.eql(u8, source.resultType, "lesson"))
                .lesson
            else
                return error.InvalidSearchPage;
            const valid_course = result_type == .course and
                std.mem.eql(u8, source.id, source.courseId) and
                std.mem.eql(u8, source.name, source.courseName) and
                source.sectionId.len == 0 and
                source.sectionName.len == 0 and
                source.lessonOffset == 0 and
                std.mem.eql(u8, source.kind, "course");
            const valid_lesson = result_type == .lesson and
                validRequiredText(source.sectionId, max_section_id_bytes) and
                validRequiredText(source.sectionName, max_section_name_bytes) and
                validLessonKind(source.kind);
            if (!validRequiredText(source.id, max_lesson_id_bytes) or
                !validRequiredText(source.courseId, max_course_id_bytes) or
                !validRequiredText(source.courseName, max_course_name_bytes) or
                !validRequiredText(source.name, max_lesson_name_bytes) or
                (!valid_course and !valid_lesson) or
                source.score <= 0)
            {
                return error.InvalidSearchPage;
            }
            for (results[0..index]) |result| {
                if (result.result_type == result_type and
                    std.mem.eql(u8, result.id(), source.id)) return error.InvalidSearchPage;
            }
            const action_prefix = if (result_type == .course) "course/" else "lesson/";
            results[index] = .{
                .result_type = result_type,
                .action_key_len = action_prefix.len + source.id.len,
                .id_len = source.id.len,
                .course_id_len = source.courseId.len,
                .course_name_len = source.courseName.len,
                .section_name_len = source.sectionName.len,
                .name_len = source.name.len,
                .kind_len = source.kind.len,
                .lesson_offset = source.lessonOffset,
            };
            @memcpy(results[index].action_key_storage[0..action_prefix.len], action_prefix);
            @memcpy(results[index].action_key_storage[action_prefix.len..][0..source.id.len], source.id);
            @memcpy(results[index].id_storage[0..source.id.len], source.id);
            @memcpy(results[index].course_id_storage[0..source.courseId.len], source.courseId);
            @memcpy(results[index].course_name_storage[0..source.courseName.len], source.courseName);
            @memcpy(results[index].section_name_storage[0..source.sectionName.len], source.sectionName);
            @memcpy(results[index].name_storage[0..source.name.len], source.name);
            @memcpy(results[index].kind_storage[0..source.kind.len], source.kind);
        }

        @memcpy(model.search_results[0..page.rows.len], results[0..page.rows.len]);
        model.search_result_count = page.rows.len;
        model.search_offset = page.offset;
        model.pending_search_offset = page.offset;
        model.total_search_results = page.total;
        model.search_message_len = 0;
        model.search_state = if (page.total == 0) .empty else .results;
    }
};

fn validRequiredText(value: []const u8, max_bytes: usize) bool {
    return value.len != 0 and
        value.len <= max_bytes and
        std.unicode.utf8ValidateSlice(value) and
        std.mem.indexOfScalar(u8, value, 0) == null;
}

fn validLessonKind(value: []const u8) bool {
    return std.mem.eql(u8, value, "video") or
        std.mem.eql(u8, value, "audio") or
        std.mem.eql(u8, value, "document") or
        std.mem.eql(u8, value, "quiz");
}

fn staleSearchIndex(bytes: []const u8) bool {
    const StalePayload = struct {
        @"error": []const u8,
        expected: u64,
        actual: ?u64,
    };
    const parsed = std.json.parseFromSlice(StalePayload, std.heap.page_allocator, bytes, .{
        .ignore_unknown_fields = true,
    }) catch return false;
    defer parsed.deinit();
    return parsed.value.expected != 0 and std.mem.eql(u8, parsed.value.@"error", "staleSearchIndex");
}

pub const Msg = union(enum) {
    appearance_changed: native_sdk.Appearance,
    library_loaded: native_sdk.EffectExternalResult,
    course_accessed: native_sdk.EffectExternalResult,
    lessons_loaded: native_sdk.EffectExternalResult,
    search_indexed: native_sdk.EffectExternalResult,
    search_loaded: native_sdk.EffectExternalResult,
    open_course: []const u8,
    toggle_section: []const u8,
    open_lesson: []const u8,
    open_search,
    dismiss_search,
    search_edited: canvas.TextInputEvent,
    open_search_result: []const u8,
    navigate_back,
    canvas_resized: f32,
    course_split_resized: f32,
    previous_page,
    next_page,
    previous_lesson_page,
    next_lesson_page,
    previous_search_page,
    next_search_page,

    pub const view_unbound = .{ "appearance_changed", "library_loaded", "course_accessed", "lessons_loaded", "search_indexed", "search_loaded", "canvas_resized" };
};

pub const LibraryUi = canvas.Ui(Msg);
pub const library_markup = @embedFile("library.native");
pub const CompiledLibraryView = canvas.CompiledMarkupView(Model, Msg, library_markup);
const library_fragments = [_]canvas.MarkupFragment{
    CompiledLibraryView.fragment("src/library.native"),
};

const dev_markup_reload = builtin.mode == .Debug;
pub const LibraryApp = native_sdk.UiAppWithFeatures(Model, Msg, .{ .runtime_markup = dev_markup_reload });
pub const Effects = LibraryApp.Effects;

pub const primary_font_id: canvas.FontId = canvas.min_registered_font_id;
pub const app_fonts = [_]LibraryApp.FontRegistration{.{
    .id = primary_font_id,
    .name = "melearner-ui.ttf",
    .ttf = @embedFile("fonts/melearner-ui.ttf"),
}};

pub fn tokensFromModel(model: *const Model) canvas.DesignTokens {
    const color_scheme: canvas.ColorScheme = switch (model.appearance.color_scheme) {
        .light => .light,
        .dark => .dark,
    };
    var tokens = canvas.DesignTokens.theme(.{
        .color_scheme = color_scheme,
        .contrast = if (model.appearance.high_contrast) .high else .standard,
        .reduce_motion = model.appearance.reduce_motion,
    });
    if (!model.appearance.high_contrast) {
        tokens.colors = switch (color_scheme) {
            .light => (canvas.ColorTokenOverrides{
                .background = canvas.Color.rgb8(245, 244, 237),
                .surface = canvas.Color.rgb8(250, 249, 245),
                .surface_subtle = canvas.Color.rgb8(232, 230, 220),
                .surface_pressed = canvas.Color.rgb8(228, 236, 245),
                .text = canvas.Color.rgb8(20, 20, 19),
                .text_muted = canvas.Color.rgb8(100, 97, 89),
                .border = canvas.Color.rgb8(227, 224, 211),
                .accent = canvas.Color.rgb8(27, 54, 93),
                .accent_text = canvas.Color.rgb8(250, 249, 245),
                .destructive = canvas.Color.rgb8(169, 67, 69),
                .destructive_text = canvas.Color.rgb8(250, 249, 245),
                .focus_ring = canvas.Color.rgb8(27, 54, 93),
                .disabled = canvas.Color.rgb8(238, 236, 227),
            }).apply(tokens.colors),
            .dark => (canvas.ColorTokenOverrides{
                .background = canvas.Color.rgb8(16, 17, 19),
                .surface = canvas.Color.rgb8(23, 23, 25),
                .surface_subtle = canvas.Color.rgb8(36, 35, 33),
                .surface_pressed = canvas.Color.rgb8(38, 52, 71),
                .text = canvas.Color.rgb8(235, 232, 223),
                .text_muted = canvas.Color.rgb8(166, 160, 149),
                .border = canvas.Color.rgb8(45, 44, 41),
                .accent = canvas.Color.rgb8(158, 184, 220),
                .accent_text = canvas.Color.rgb8(16, 17, 19),
                .destructive = canvas.Color.rgb8(211, 107, 112),
                .destructive_text = canvas.Color.rgb8(31, 14, 16),
                .focus_ring = canvas.Color.rgb8(158, 184, 220),
                .disabled = canvas.Color.rgb8(34, 34, 34),
            }).apply(tokens.colors),
        };
    }
    tokens.typography.font_id = primary_font_id;
    tokens.metrics.control_height_sm = 40;
    tokens.metrics.control_height = 40;
    tokens.metrics.control_height_lg = 48;
    tokens.metrics.row_extent = 40;
    if (!model.appearance.reduce_motion) {
        tokens.motion.fast_ms = 150;
        tokens.motion.normal_ms = 200;
        tokens.motion.slow_ms = 250;
    }
    return tokens;
}

pub fn rootView(ui: *LibraryUi, model: *const Model) LibraryUi.Node {
    var content = CompiledLibraryView.build(ui, model);
    content.widget.layout.grow = 1;
    content.widget.layout.max_size.width = content_max_width;
    return ui.row(.{
        .grow = 1,
        .main = .center,
        .style_tokens = .{ .background = .background },
    }, .{content});
}

pub fn navigationDepth(model: *const Model) usize {
    if (model.screen == .library) return 0;
    if (model.canvas_width < course_split_breakpoint and model.compact_course_page == .lesson) return 2;
    return 1;
}

pub fn onFrame(model: *const Model, frame: native_sdk.platform.GpuFrame) ?Msg {
    if (frame.size.width != model.canvas_width) return .{ .canvas_resized = frame.size.width };
    return null;
}

pub const cmd_search = "melearner.search";
pub const cmd_dismiss = "melearner.dismiss";

pub fn onCommand(name: []const u8) ?Msg {
    if (std.mem.eql(u8, name, cmd_search)) return .open_search;
    if (std.mem.eql(u8, name, cmd_dismiss)) return .dismiss_search;
    return null;
}

pub fn appOptions() LibraryApp.Options {
    return .{
        .name = "melearner",
        .scene = shell_scene,
        .canvas_label = canvas_label,
        .update_fx = update,
        .init_fx = boot,
        .tokens_fn = tokensFromModel,
        .fonts = &app_fonts,
        .on_appearance = onAppearance,
        .on_command = onCommand,
        .on_frame = onFrame,
        .navigation_depth_fn = navigationDepth,
        .view = rootView,
    };
}

pub fn boot(model: *Model, effects: *Effects) void {
    model.screen = .library;
    model.course_state = .inactive;
    model.library_revision = 0;
    model.total_courses = 0;
    model.page_offset = 0;
    model.library_request_id = 0;
    model.course_access_request_id = 0;
    model.lesson_page_request_id = 0;
    model.course_count = 0;
    model.search_open = false;
    model.restore_search_focus = false;
    model.search_state = .inactive;
    model.search_query_buffer.clear();
    model.search_index_revision = 0;
    model.search_index_request_id = 0;
    model.search_query_request_id = 0;
    model.search_query_key = 0;
    model.desired_search_query_id = 0;
    model.active_search_query_id = 0;
    model.clearSearchResults();
    requestLibraryPage(model, effects, 0, 0);
}

fn requestLibraryPage(model: *Model, effects: *Effects, expected_revision: u64, offset: u64) void {
    var payload_storage: [core_adapter.library_page_request_bytes]u8 = undefined;
    const payload = core_adapter.encodeLibraryPageRequest(&payload_storage, expected_revision, offset);
    model.library_state = .opening;
    model.pending_page_offset = offset;
    model.library_request_id = effects.external(.{
        .key = core_adapter.library_page_key,
        .adapter_id = core_adapter.adapter_id,
        .kind = @intFromEnum(core_adapter.Operation.load_library_page),
        .schema_version = core_adapter.schema_version,
        .payload = payload,
        .on_result = Effects.externalMsg(.library_loaded),
    }) catch {
        model.setFailure("The Library service could not start.");
        return;
    };
}

fn requestCourseAccess(model: *Model, effects: *Effects) void {
    var payload_storage: [core_adapter.course_access_request_header_bytes + max_course_id_bytes]u8 = undefined;
    const payload = core_adapter.encodeCourseAccessRequest(
        &payload_storage,
        model.library_revision,
        model.selected_course.id(),
    ) catch {
        model.setCourseFailure("The Course request was invalid.");
        return;
    };
    model.course_state = .accessing;
    model.course_access_request_id = effects.external(.{
        .key = core_adapter.course_access_key,
        .adapter_id = core_adapter.adapter_id,
        .kind = @intFromEnum(core_adapter.Operation.access_course),
        .schema_version = core_adapter.schema_version,
        .payload = payload,
        .on_result = Effects.externalMsg(.course_accessed),
    }) catch {
        model.setCourseFailure("The Course service could not start.");
        return;
    };
}

fn requestLessonPage(model: *Model, effects: *Effects, offset: u64) void {
    var payload_storage: [core_adapter.lesson_page_request_header_bytes + max_course_id_bytes]u8 = undefined;
    const payload = core_adapter.encodeLessonPageRequest(
        &payload_storage,
        model.library_revision,
        offset,
        model.selected_course.id(),
    ) catch {
        model.setCourseFailure("The Lesson request was invalid.");
        return;
    };
    model.course_state = .loading;
    model.pending_lesson_page_offset = offset;
    model.lesson_page_request_id = effects.external(.{
        .key = core_adapter.lesson_page_key,
        .adapter_id = core_adapter.adapter_id,
        .kind = @intFromEnum(core_adapter.Operation.load_lesson_page),
        .schema_version = core_adapter.schema_version,
        .payload = payload,
        .on_result = Effects.externalMsg(.lessons_loaded),
    }) catch {
        model.setCourseFailure("The Lesson service could not start.");
        return;
    };
}

fn requestSearchIndex(model: *Model, effects: *Effects) void {
    if (model.search_index_request_id != 0 or model.library_revision == 0) return;
    var payload_storage: [core_adapter.search_index_request_bytes]u8 = undefined;
    const payload = core_adapter.encodeSearchIndexRequest(&payload_storage, model.library_revision) catch {
        model.setSearchFailure("Search could not prepare this Library.");
        return;
    };
    model.search_state = .building;
    model.search_index_request_id = effects.external(.{
        .key = core_adapter.search_index_key,
        .adapter_id = core_adapter.adapter_id,
        .kind = @intFromEnum(core_adapter.Operation.rebuild_search_index),
        .schema_version = core_adapter.schema_version,
        .payload = payload,
        .on_result = Effects.externalMsg(.search_indexed),
    }) catch {
        model.setSearchFailure("Search could not prepare this Library.");
        return;
    };
}

fn requestSearchPage(model: *Model, effects: *Effects, query_id: u64, offset: u64) void {
    const query = std.mem.trim(u8, model.searchQuery(), " \t\r\n");
    if (model.search_index_revision == 0 or query_id == 0 or query.len == 0 or
        query_id > std.math.maxInt(u64) - core_adapter.search_query_key_base)
    {
        return;
    }
    var payload_storage: [core_adapter.search_query_request_header_bytes + core_adapter.max_search_query_bytes]u8 = undefined;
    const payload = core_adapter.encodeSearchQueryRequest(
        &payload_storage,
        model.search_index_revision,
        query_id,
        offset,
        query,
    ) catch {
        model.setSearchFailure("The search query was invalid.");
        return;
    };
    const key = core_adapter.search_query_key_base + query_id;
    model.search_state = .querying;
    model.active_search_query_id = query_id;
    model.pending_search_offset = offset;
    model.search_query_key = key;
    model.search_query_request_id = effects.external(.{
        .key = key,
        .adapter_id = core_adapter.adapter_id,
        .kind = @intFromEnum(core_adapter.Operation.query_search),
        .schema_version = core_adapter.schema_version,
        .payload = payload,
        .on_result = Effects.externalMsg(.search_loaded),
    }) catch {
        model.setSearchFailure("Search could not start.");
        return;
    };
}

fn cancelSearchWork(model: *Model, effects: *Effects) void {
    if (model.search_index_request_id != 0) effects.cancel(core_adapter.search_index_key);
    if (model.search_query_request_id != 0) effects.cancel(model.search_query_key);
    model.search_open = false;
    model.search_index_request_id = 0;
    model.search_query_request_id = 0;
    model.search_query_key = 0;
    model.active_search_query_id = 0;
}

pub fn update(model: *Model, msg: Msg, effects: *Effects) void {
    switch (msg) {
        .appearance_changed => |appearance| model.appearance = appearance,
        .library_loaded => |result| {
            if (model.screen != .library or result.request_id != model.library_request_id) return;
            model.library_request_id = 0;
            if (result.key != core_adapter.library_page_key or
                result.adapter_id != core_adapter.adapter_id or
                result.kind != @intFromEnum(core_adapter.Operation.load_library_page) or
                result.schema_version != core_adapter.schema_version)
            {
                model.setFailure("The Library service returned an unexpected response.");
                return;
            }
            switch (result.outcome) {
                .ok => model.loadCoursePage(result.bytes) catch
                    model.setFailure("The Library service returned invalid data."),
                .failed => model.setFailure(result.bytes),
                .cancelled => model.setFailure("Opening the Library was cancelled."),
                .adapter_unavailable, .submit_failed => model.setFailure("The Library service is unavailable."),
            }
        },
        .course_accessed => |result| {
            if (model.screen != .course or
                model.course_state != .accessing or
                result.request_id != model.course_access_request_id) return;
            model.course_access_request_id = 0;
            if (result.key != core_adapter.course_access_key or
                result.adapter_id != core_adapter.adapter_id or
                result.kind != @intFromEnum(core_adapter.Operation.access_course) or
                result.schema_version != core_adapter.schema_version)
            {
                model.setCourseFailure("The Course service returned an unexpected response.");
                return;
            }
            switch (result.outcome) {
                .ok => {
                    model.loadCourseAccess(result.bytes) catch {
                        model.setCourseFailure("The Course service returned invalid data.");
                        return;
                    };
                    const target_offset = model.target_lesson_offset -
                        (model.target_lesson_offset % core_adapter.lesson_page_size);
                    requestLessonPage(model, effects, target_offset);
                },
                .failed => model.setCourseFailure(result.bytes),
                .cancelled => model.setCourseFailure("Opening the Course was cancelled."),
                .adapter_unavailable, .submit_failed => model.setCourseFailure("The Course service is unavailable."),
            }
        },
        .lessons_loaded => |result| {
            if (model.screen != .course or
                model.course_state != .loading or
                result.request_id != model.lesson_page_request_id) return;
            model.lesson_page_request_id = 0;
            if (result.key != core_adapter.lesson_page_key or
                result.adapter_id != core_adapter.adapter_id or
                result.kind != @intFromEnum(core_adapter.Operation.load_lesson_page) or
                result.schema_version != core_adapter.schema_version)
            {
                model.setCourseFailure("The Lesson service returned an unexpected response.");
                return;
            }
            switch (result.outcome) {
                .ok => model.loadLessonPage(result.bytes) catch
                    model.setCourseFailure("The Lesson service returned invalid data."),
                .failed => model.setCourseFailure(result.bytes),
                .cancelled => model.setCourseFailure("Loading the Lessons was cancelled."),
                .adapter_unavailable, .submit_failed => model.setCourseFailure("The Lesson service is unavailable."),
            }
        },
        .search_indexed => |result| {
            if (result.request_id != model.search_index_request_id) return;
            model.search_index_request_id = 0;
            if (result.key != core_adapter.search_index_key or
                result.adapter_id != core_adapter.adapter_id or
                result.kind != @intFromEnum(core_adapter.Operation.rebuild_search_index) or
                result.schema_version != core_adapter.schema_version)
            {
                model.setSearchFailure("Search returned an unexpected response.");
                return;
            }
            switch (result.outcome) {
                .ok => {
                    model.loadSearchIndex(result.bytes) catch {
                        model.setSearchFailure("Search returned invalid index data.");
                        return;
                    };
                    if (model.search_open and model.desired_search_query_id != 0 and
                        !model.search_query_buffer.isEmpty())
                    {
                        requestSearchPage(model, effects, model.desired_search_query_id, 0);
                    }
                },
                .failed => model.setSearchFailure(result.bytes),
                .cancelled => {
                    if (model.search_open and model.screen == .library) requestSearchIndex(model, effects);
                },
                .adapter_unavailable, .submit_failed => model.setSearchFailure("Search is unavailable."),
            }
        },
        .search_loaded => |result| {
            if (result.request_id != model.search_query_request_id) return;
            if (result.key != model.search_query_key or
                result.adapter_id != core_adapter.adapter_id or
                result.kind != @intFromEnum(core_adapter.Operation.query_search) or
                result.schema_version != core_adapter.schema_version)
            {
                model.setSearchFailure("Search returned an unexpected response.");
                return;
            }
            const completed_query_id = model.active_search_query_id;
            switch (result.outcome) {
                .ok => {
                    if (!model.search_open or completed_query_id != model.desired_search_query_id) {
                        model.search_query_request_id = 0;
                        model.search_query_key = 0;
                        model.active_search_query_id = 0;
                        if (!model.search_open) {
                            model.search_state = if (model.search_index_revision == 0) .building else .ready;
                        } else if (model.desired_search_query_id != 0 and
                            !model.search_query_buffer.isEmpty())
                        {
                            requestSearchPage(model, effects, model.desired_search_query_id, 0);
                        }
                        return;
                    }
                    model.loadSearchPage(result.bytes) catch {
                        model.setSearchFailure("Search returned invalid result data.");
                        return;
                    };
                    model.search_query_request_id = 0;
                    model.search_query_key = 0;
                    model.active_search_query_id = 0;
                },
                .failed => {
                    model.search_query_request_id = 0;
                    model.search_query_key = 0;
                    model.active_search_query_id = 0;
                    if (staleSearchIndex(result.bytes)) {
                        model.search_index_revision = 0;
                        requestSearchIndex(model, effects);
                    } else {
                        model.setSearchFailure(result.bytes);
                    }
                },
                .cancelled => {
                    model.search_query_request_id = 0;
                    model.search_query_key = 0;
                    model.active_search_query_id = 0;
                    if (model.search_open and model.desired_search_query_id != 0 and
                        !model.search_query_buffer.isEmpty())
                    {
                        if (model.search_index_revision == 0)
                            requestSearchIndex(model, effects)
                        else
                            requestSearchPage(model, effects, model.desired_search_query_id, 0);
                    } else if (!model.search_open) {
                        model.search_state = if (model.search_index_revision == 0) .building else .ready;
                    }
                },
                .adapter_unavailable, .submit_failed => model.setSearchFailure("Search is unavailable."),
            }
        },
        .open_search => {
            if (model.screen != .library or (!model.libraryReady() and !model.libraryEmpty())) return;
            model.search_open = true;
            model.restore_search_focus = false;
            if (model.search_index_revision == 0 and model.search_index_request_id == 0) {
                requestSearchIndex(model, effects);
            } else if (model.search_index_revision != 0 and
                model.search_query_request_id == 0 and
                model.desired_search_query_id != 0 and
                model.search_state != .results and
                model.search_state != .empty and
                !model.search_query_buffer.isEmpty())
            {
                requestSearchPage(model, effects, model.desired_search_query_id, 0);
            }
        },
        .dismiss_search => {
            if (!model.search_open) return;
            model.search_open = false;
            model.restore_search_focus = true;
            if (model.search_query_request_id != 0) {
                effects.cancel(model.search_query_key);
                model.search_state = if (model.search_index_revision == 0) .building else .ready;
            }
        },
        .search_edited => |edit| {
            if (!model.search_open or model.screen != .library) return;
            model.search_query_buffer.apply(edit);
            model.clearSearchResults();
            if (model.search_query_buffer.isEmpty()) {
                model.desired_search_query_id = 0;
                if (model.search_query_request_id != 0) effects.cancel(model.search_query_key);
                model.search_state = if (model.search_index_revision == 0) .building else .ready;
                return;
            }
            const query_id = model.next_search_query_id;
            model.next_search_query_id +%= 1;
            if (model.next_search_query_id == 0) model.next_search_query_id = 1;
            model.desired_search_query_id = query_id;
            if (model.search_query_request_id != 0) {
                effects.cancel(model.search_query_key);
            } else if (model.search_index_revision != 0) {
                requestSearchPage(model, effects, query_id, 0);
            } else if (model.search_index_request_id == 0) {
                requestSearchIndex(model, effects);
            }
        },
        .open_search_result => |action_key| {
            if (!model.search_open or model.screen != .library or model.search_state != .results) return;
            const separator = std.mem.indexOfScalar(u8, action_key, '/') orelse return;
            const result_type: SearchResultType = if (std.mem.eql(u8, action_key[0..separator], "course"))
                .course
            else if (std.mem.eql(u8, action_key[0..separator], "lesson"))
                .lesson
            else
                return;
            const result_id = action_key[separator + 1 ..];
            if (result_id.len == 0) return;
            const result = for (model.search_results[0..model.search_result_count]) |*candidate| {
                if (candidate.result_type == result_type and
                    std.mem.eql(u8, candidate.id(), result_id)) break candidate;
            } else return;
            const preserve_selected_lesson = result_type == .course and
                model.has_selected_lesson and
                std.mem.eql(u8, model.selected_course.id(), result.courseId());
            model.selected_course = .{
                .id_len = result.course_id_len,
                .name_len = result.course_name_len,
                .available = true,
            };
            @memcpy(model.selected_course.id_storage[0..result.course_id_len], result.courseId());
            @memcpy(model.selected_course.name_storage[0..result.course_name_len], result.courseName());
            if (result_type == .lesson) {
                model.has_selected_lesson = false;
                model.selected_lesson_offset = 0;
                model.setTargetLesson(result.id(), result.lesson_offset);
            } else if (preserve_selected_lesson) {
                model.setTargetLesson(model.selected_lesson.id(), model.selected_lesson_offset);
            } else {
                model.has_selected_lesson = false;
                model.selected_lesson_offset = 0;
                model.clearTargetLesson();
            }
            cancelSearchWork(model, effects);
            model.restore_search_focus = false;
            model.screen = .course;
            model.compact_course_page = if (result_type == .lesson) .lesson else .outline;
            model.total_lessons = 0;
            model.lesson_count = 0;
            model.course_message_len = 0;
            requestCourseAccess(model, effects);
        },
        .open_course => |course_id| {
            if (model.screen != .library or !model.libraryReady()) return;
            const course = for (model.courses[0..model.course_count]) |*candidate| {
                if (std.mem.eql(u8, candidate.id(), course_id)) break candidate;
            } else return;
            if (!course.available) return;
            cancelSearchWork(model, effects);
            const reopening_selected_course = model.has_selected_lesson and
                std.mem.eql(u8, model.selected_course.id(), course.id());
            model.selected_course = course.*;
            if (reopening_selected_course) {
                model.setTargetLesson(model.selected_lesson.id(), model.selected_lesson_offset);
            } else {
                model.has_selected_lesson = false;
                model.selected_lesson_offset = 0;
                model.clearTargetLesson();
            }
            model.total_lessons = 0;
            model.lesson_page_offset = 0;
            model.pending_lesson_page_offset = 0;
            model.lesson_count = 0;
            model.course_message_len = 0;
            model.course_access_request_id = 0;
            model.lesson_page_request_id = 0;
            model.screen = .course;
            model.restore_course_focus = false;
            model.compact_course_page = .outline;
            requestCourseAccess(model, effects);
        },
        .toggle_section => |section_id| {
            if (model.screen != .course or !model.courseReady()) return;
            if (model.has_selected_lesson and
                std.mem.eql(u8, model.selected_lesson.sectionId(), section_id)) return;
            const section = for (model.lessons[0..model.lesson_count]) |*lesson| {
                if (lesson.starts_section and std.mem.eql(u8, lesson.sectionId(), section_id)) break lesson;
            } else return;
            const expanded = !section.section_expanded;
            for (model.lessons[0..model.lesson_count]) |*lesson| {
                if (std.mem.eql(u8, lesson.sectionId(), section_id)) lesson.section_expanded = expanded;
            }
        },
        .open_lesson => |lesson_id| {
            if (model.screen != .course or !model.courseReady()) return;
            const lesson = for (model.lessons[0..model.lesson_count], 0..) |*candidate, index| {
                if (std.mem.eql(u8, candidate.id(), lesson_id)) {
                    model.selected_lesson_offset = model.lesson_page_offset + @as(u64, @intCast(index));
                    break candidate;
                }
            } else return;
            for (model.lessons[0..model.lesson_count]) |*candidate| {
                candidate.selected = false;
                candidate.restore_focus = false;
                if (std.mem.eql(u8, candidate.sectionId(), lesson.sectionId())) {
                    candidate.section_expanded = true;
                }
            }
            lesson.selected = true;
            model.selected_lesson = lesson.*;
            model.has_selected_lesson = true;
            model.compact_course_page = .lesson;
        },
        .navigate_back => {
            if (model.screen != .course) return;
            if (model.canvas_width < course_split_breakpoint and model.compact_course_page == .lesson) {
                model.compact_course_page = .outline;
                for (model.lessons[0..model.lesson_count]) |*lesson| {
                    lesson.restore_focus = std.mem.eql(u8, lesson.id(), model.selected_lesson.id());
                }
                return;
            }
            effects.cancel(core_adapter.course_access_key);
            effects.cancel(core_adapter.lesson_page_key);
            model.screen = .library;
            model.restore_course_focus = true;
            model.course_state = .inactive;
            model.total_lessons = 0;
            model.lesson_page_offset = 0;
            model.pending_lesson_page_offset = 0;
            model.lesson_count = 0;
            model.course_message_len = 0;
            model.course_access_request_id = 0;
            model.lesson_page_request_id = 0;
            model.library_revision = 0;
            requestLibraryPage(model, effects, 0, 0);
        },
        .canvas_resized => |width| {
            if (std.math.isFinite(width) and width > 0) model.canvas_width = width;
        },
        .course_split_resized => |fraction| {
            if (std.math.isFinite(fraction)) model.course_split_fraction = std.math.clamp(fraction, 0.28, 0.55);
        },
        .previous_page => {
            if (!model.hasPreviousPage()) return;
            requestLibraryPage(
                model,
                effects,
                model.library_revision,
                model.page_offset - core_adapter.library_page_size,
            );
        },
        .next_page => {
            if (!model.hasNextPage()) return;
            requestLibraryPage(
                model,
                effects,
                model.library_revision,
                model.page_offset + core_adapter.library_page_size,
            );
        },
        .previous_lesson_page => {
            if (!model.hasPreviousLessonPage()) return;
            requestLessonPage(model, effects, model.lesson_page_offset - core_adapter.lesson_page_size);
        },
        .next_lesson_page => {
            if (!model.hasNextLessonPage()) return;
            requestLessonPage(model, effects, model.lesson_page_offset + core_adapter.lesson_page_size);
        },
        .previous_search_page => {
            if (!model.hasPreviousSearchPage() or model.search_query_request_id != 0) return;
            requestSearchPage(
                model,
                effects,
                model.desired_search_query_id,
                model.search_offset - core_adapter.search_page_size,
            );
        },
        .next_search_page => {
            if (!model.hasNextSearchPage() or model.search_query_request_id != 0) return;
            requestSearchPage(
                model,
                effects,
                model.desired_search_query_id,
                model.search_offset + core_adapter.search_page_size,
            );
        },
    }
}

pub fn onAppearance(appearance: native_sdk.Appearance) ?Msg {
    return .{ .appearance_changed = appearance };
}

pub fn main(init: std.process.Init) !void {
    var state_dir_buffer: [std.Io.Dir.max_path_bytes]u8 = undefined;
    const state_dir = try native_sdk.app_dirs.resolveOne(
        .{ .name = "melearner" },
        native_sdk.app_dirs.currentPlatform(),
        native_sdk.debug.envFromMap(init.environ_map),
        .data,
        &state_dir_buffer,
    );
    const adapter = try core_adapter.CoreAdapter.create(std.heap.page_allocator, init.io, state_dir);
    defer adapter.destroy();

    const app_state = try std.heap.page_allocator.create(LibraryApp);
    defer std.heap.page_allocator.destroy(app_state);
    var options = appOptions();
    if (dev_markup_reload) {
        options.fragment_watch = .{ .fragments = &library_fragments, .io = init.io };
    }
    app_state.* = LibraryApp.init(std.heap.page_allocator, .{}, options);
    defer app_state.deinit();
    app_state.effects.bindExternalAdapter(adapter.binding());

    try runner.runWithOptions(app_state.app(), .{
        .app_name = "melearner",
        .window_title = "melearner",
        .bundle_id = "com.whitehades.melearner",
        .default_frame = geometry.RectF.init(0, 0, window_width, window_height),
        .restore_state = false,
        .js_window_api = false,
    }, init);
}

test {
    _ = @import("tests.zig");
}
