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
const max_courses: usize = core_adapter.library_page_size;
const max_lessons: usize = core_adapter.lesson_page_size;
const max_course_id_bytes = core_adapter.max_course_id_bytes;
const max_course_name_bytes = 512;
const max_lesson_id_bytes = 128;
const max_lesson_name_bytes = 512;
const max_section_id_bytes = 128;
const max_section_name_bytes = 512;
const max_lesson_kind_bytes = 16;
const max_library_message_bytes = 256;
const max_course_message_bytes = 256;

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

    pub fn id(lesson: *const Lesson) []const u8 {
        return lesson.id_storage[0..lesson.id_len];
    }

    fn sectionId(lesson: *const Lesson) []const u8 {
        return lesson.section_id_storage[0..lesson.section_id_len];
    }

    pub fn sectionName(lesson: *const Lesson) []const u8 {
        return lesson.section_name_storage[0..lesson.section_name_len];
    }

    pub fn name(lesson: *const Lesson) []const u8 {
        return lesson.name_storage[0..lesson.name_len];
    }

    pub fn kindLabel(lesson: *const Lesson) []const u8 {
        return if (std.mem.eql(u8, lesson.kind_storage[0..lesson.kind_len], "video"))
            "Video"
        else if (std.mem.eql(u8, lesson.kind_storage[0..lesson.kind_len], "audio"))
            "Audio"
        else
            "Document";
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

pub const Model = struct {
    appearance: native_sdk.Appearance = .{},
    library_state: LibraryState = .opening,
    library_revision: u64 = 0,
    total_courses: u64 = 0,
    page_offset: u64 = 0,
    pending_page_offset: u64 = 0,
    pending_request_id: u64 = 0,
    courses: [max_courses]Course = undefined,
    course_count: usize = 0,
    library_message_storage: [max_library_message_bytes]u8 = [_]u8{0} ** max_library_message_bytes,
    library_message_len: usize = 0,
    screen: Screen = .library,
    course_state: CourseState = .inactive,
    selected_course: Course = .{},
    total_lessons: u64 = 0,
    lesson_page_offset: u64 = 0,
    pending_lesson_page_offset: u64 = 0,
    lessons: [max_lessons]Lesson = undefined,
    lesson_count: usize = 0,
    course_message_storage: [max_course_message_bytes]u8 = [_]u8{0} ** max_course_message_bytes,
    course_message_len: usize = 0,

    pub const view_unbound = .{
        "appearance",
        "library_state",
        "library_revision",
        "total_courses",
        "page_offset",
        "pending_page_offset",
        "pending_request_id",
        "courses",
        "course_count",
        "library_message_storage",
        "library_message_len",
        "screen",
        "course_state",
        "selected_course",
        "total_lessons",
        "lesson_page_offset",
        "pending_lesson_page_offset",
        "lessons",
        "lesson_count",
        "course_message_storage",
        "course_message_len",
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
        model.pending_request_id = 0;
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
        model.pending_request_id = 0;
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
    }

    fn loadCourseAccess(model: *Model, bytes: []const u8) !void {
        const AccessPayload = struct {
            revision: u64,
            courseId: []const u8,
            lastAccessed: []const u8,
        };
        const parsed = try std.json.parseFromSlice(AccessPayload, std.heap.page_allocator, bytes, .{
            .ignore_unknown_fields = true,
        });
        defer parsed.deinit();
        const access = parsed.value;
        if (model.course_state != .accessing or
            access.revision <= model.library_revision or
            !std.mem.eql(u8, access.courseId, model.selected_course.id()) or
            access.lastAccessed.len == 0 or
            access.lastAccessed.len > 64 or
            !std.mem.endsWith(u8, access.lastAccessed, "Z"))
        {
            return error.InvalidCourseAccess;
        }
        model.library_revision = access.revision;
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

        @memcpy(model.lessons[0..page.rows.len], lessons[0..page.rows.len]);
        model.lesson_count = page.rows.len;
        model.total_lessons = page.total;
        model.lesson_page_offset = page.offset;
        model.pending_lesson_page_offset = page.offset;
        model.course_message_len = 0;
        model.course_state = if (page.total == 0) .empty else .ready;
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
        std.mem.eql(u8, value, "document");
}

pub const Msg = union(enum) {
    appearance_changed: native_sdk.Appearance,
    library_loaded: native_sdk.EffectExternalResult,
    course_accessed: native_sdk.EffectExternalResult,
    lessons_loaded: native_sdk.EffectExternalResult,
    open_course: []const u8,
    back_to_library,
    previous_page,
    next_page,
    previous_lesson_page,
    next_lesson_page,

    pub const view_unbound = .{ "appearance_changed", "library_loaded", "course_accessed", "lessons_loaded" };
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
        .view = rootView,
    };
}

pub fn boot(model: *Model, effects: *Effects) void {
    model.screen = .library;
    model.course_state = .inactive;
    model.library_revision = 0;
    model.total_courses = 0;
    model.page_offset = 0;
    model.pending_request_id = 0;
    model.course_count = 0;
    requestLibraryPage(model, effects, 0, 0);
}

fn requestLibraryPage(model: *Model, effects: *Effects, expected_revision: u64, offset: u64) void {
    var payload_storage: [core_adapter.library_page_request_bytes]u8 = undefined;
    const payload = core_adapter.encodeLibraryPageRequest(&payload_storage, expected_revision, offset);
    model.library_state = .opening;
    model.pending_page_offset = offset;
    model.pending_request_id = effects.external(.{
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
    model.pending_request_id = effects.external(.{
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
    model.pending_request_id = effects.external(.{
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

pub fn update(model: *Model, msg: Msg, effects: *Effects) void {
    switch (msg) {
        .appearance_changed => |appearance| model.appearance = appearance,
        .library_loaded => |result| {
            if (model.screen != .library or result.request_id != model.pending_request_id) return;
            model.pending_request_id = 0;
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
                result.request_id != model.pending_request_id) return;
            model.pending_request_id = 0;
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
                    requestLessonPage(model, effects, 0);
                },
                .failed => model.setCourseFailure(result.bytes),
                .cancelled => model.setCourseFailure("Opening the Course was cancelled."),
                .adapter_unavailable, .submit_failed => model.setCourseFailure("The Course service is unavailable."),
            }
        },
        .lessons_loaded => |result| {
            if (model.screen != .course or
                model.course_state != .loading or
                result.request_id != model.pending_request_id) return;
            model.pending_request_id = 0;
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
        .open_course => |course_id| {
            if (model.screen != .library or !model.libraryReady()) return;
            const course = for (model.courses[0..model.course_count]) |*candidate| {
                if (std.mem.eql(u8, candidate.id(), course_id)) break candidate;
            } else return;
            if (!course.available) return;
            model.selected_course = course.*;
            model.total_lessons = 0;
            model.lesson_page_offset = 0;
            model.pending_lesson_page_offset = 0;
            model.lesson_count = 0;
            model.course_message_len = 0;
            model.pending_request_id = 0;
            model.screen = .course;
            requestCourseAccess(model, effects);
        },
        .back_to_library => {
            if (model.screen != .course) return;
            effects.cancel(core_adapter.course_access_key);
            effects.cancel(core_adapter.lesson_page_key);
            model.screen = .library;
            model.course_state = .inactive;
            model.total_lessons = 0;
            model.lesson_page_offset = 0;
            model.pending_lesson_page_offset = 0;
            model.lesson_count = 0;
            model.course_message_len = 0;
            model.pending_request_id = 0;
            model.library_revision = 0;
            requestLibraryPage(model, effects, 0, 0);
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
