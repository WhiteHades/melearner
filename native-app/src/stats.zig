const std = @import("std");
const core_adapter = @import("core_adapter.zig");

const max_media_rows = 4;
const max_course_rows = 4;
const max_course_id_bytes = core_adapter.max_course_id_bytes;
const max_course_name_bytes = 512;
const max_lesson_kind_bytes = 16;
const max_message_bytes = 256;
pub const activity_day_count = 84;

pub const ActivityRow = struct {
    date_storage: [10]u8 = [_]u8{0} ** 10,
    epoch_day: u64 = 0,
    watched_seconds: u64 = 0,
    lessons_touched: u64 = 0,
    completions: u64 = 0,
    level: u3 = 0,

    pub fn date(row: *const ActivityRow) []const u8 {
        return &row.date_storage;
    }

    pub fn levelZero(row: *const ActivityRow) bool {
        return row.level == 0;
    }

    pub fn levelOne(row: *const ActivityRow) bool {
        return row.level == 1;
    }

    pub fn levelTwo(row: *const ActivityRow) bool {
        return row.level == 2;
    }

    pub fn levelThree(row: *const ActivityRow) bool {
        return row.level == 3;
    }

    pub fn levelFour(row: *const ActivityRow) bool {
        return row.level == 4;
    }

    pub fn label(row: *const ActivityRow, arena: std.mem.Allocator) []const u8 {
        return std.fmt.allocPrint(arena, "{s}: {s} seconds watched, {s} Lessons touched, {s} completions", .{
            row.date(),
            formatCount(arena, row.watched_seconds),
            formatCount(arena, row.lessons_touched),
            formatCount(arena, row.completions),
        }) catch "";
    }
};

pub const MediaRow = struct {
    kind_storage: [max_lesson_kind_bytes]u8 = [_]u8{0} ** max_lesson_kind_bytes,
    kind_len: usize = 0,
    lessons: u64 = 0,
    bytes: u64 = 0,
    completed: u64 = 0,
    watched_seconds: u64 = 0,

    pub fn kind(row: *const MediaRow) []const u8 {
        return row.kind_storage[0..row.kind_len];
    }

    pub fn label(row: *const MediaRow, arena: std.mem.Allocator) []const u8 {
        return std.fmt.allocPrint(arena, "{s}: {s} of {s} completed", .{
            lessonKindLabel(row.kind()),
            formatCount(arena, row.completed),
            formatCount(arena, row.lessons),
        }) catch "";
    }

    pub fn detail(row: *const MediaRow, arena: std.mem.Allocator) []const u8 {
        return std.fmt.allocPrint(arena, "{s} bytes / {s} seconds watched", .{
            formatCount(arena, row.bytes),
            formatCount(arena, row.watched_seconds),
        }) catch "";
    }
};

pub const CourseRow = struct {
    id_storage: [max_course_id_bytes]u8 = [_]u8{0} ** max_course_id_bytes,
    id_len: usize = 0,
    name_storage: [max_course_name_bytes]u8 = [_]u8{0} ** max_course_name_bytes,
    name_len: usize = 0,
    lessons: u64 = 0,
    completed_lessons: u64 = 0,
    bytes: u64 = 0,
    watched_seconds: u64 = 0,

    pub fn id(row: *const CourseRow) []const u8 {
        return row.id_storage[0..row.id_len];
    }

    pub fn name(row: *const CourseRow) []const u8 {
        return row.name_storage[0..row.name_len];
    }

    pub fn label(row: *const CourseRow, arena: std.mem.Allocator) []const u8 {
        return std.fmt.allocPrint(arena, "{s}: {s} of {s} completed", .{
            row.name(),
            formatCount(arena, row.completed_lessons),
            formatCount(arena, row.lessons),
        }) catch "";
    }

    pub fn detail(row: *const CourseRow, arena: std.mem.Allocator) []const u8 {
        return std.fmt.allocPrint(arena, "{s} bytes / {s} seconds watched", .{
            formatCount(arena, row.bytes),
            formatCount(arena, row.watched_seconds),
        }) catch "";
    }
};

pub const State = enum {
    inactive,
    loading,
    ready,
    failed,
};

pub const Model = struct {
    open: bool = false,
    state: State = .inactive,
    snapshot_request_id: u64 = 0,
    activity_request_id: u64 = 0,
    snapshot_ready: bool = false,
    activity_ready: bool = false,
    revision: u64 = 0,
    total_courses: u64 = 0,
    available_courses: u64 = 0,
    missing_courses: u64 = 0,
    sections: u64 = 0,
    lessons: u64 = 0,
    completed_lessons: u64 = 0,
    completion_percent: u32 = 0,
    bytes: u64 = 0,
    watched_seconds: u64 = 0,
    total_seconds: u64 = 0,
    media_rows: [max_media_rows]MediaRow = undefined,
    media_count: usize = 0,
    course_rows: [max_course_rows]CourseRow = undefined,
    course_count: usize = 0,
    activity_rows: [activity_day_count]ActivityRow = undefined,
    activity_cells: [activity_day_count]ActivityRow = undefined,
    activity_count: usize = 0,
    activity_watched_seconds: u64 = 0,
    active_days: u32 = 0,
    message_storage: [max_message_bytes]u8 = [_]u8{0} ** max_message_bytes,
    message_len: usize = 0,

    pub fn message(model: *const Model) []const u8 {
        return model.message_storage[0..model.message_len];
    }

    pub fn courseLine(model: *const Model, arena: std.mem.Allocator) []const u8 {
        return std.fmt.allocPrint(arena, "{s} Courses / {s} available / {s} missing", .{
            formatCount(arena, model.total_courses),
            formatCount(arena, model.available_courses),
            formatCount(arena, model.missing_courses),
        }) catch "";
    }

    pub fn structureLine(model: *const Model, arena: std.mem.Allocator) []const u8 {
        return std.fmt.allocPrint(arena, "{s} Sections / {s} Lessons", .{
            formatCount(arena, model.sections),
            formatCount(arena, model.lessons),
        }) catch "";
    }

    pub fn progressLine(model: *const Model, arena: std.mem.Allocator) []const u8 {
        return std.fmt.allocPrint(arena, "{s} of {s} Lessons completed", .{
            formatCount(arena, model.completed_lessons),
            formatCount(arena, model.lessons),
        }) catch "";
    }

    pub fn percentLine(model: *const Model, arena: std.mem.Allocator) []const u8 {
        return std.fmt.allocPrint(arena, "{d}% complete", .{model.completion_percent}) catch "";
    }

    pub fn timeLine(model: *const Model, arena: std.mem.Allocator) []const u8 {
        return std.fmt.allocPrint(arena, "{s} of {s} seconds watched", .{
            formatCount(arena, model.watched_seconds),
            formatCount(arena, model.total_seconds),
        }) catch "";
    }

    pub fn storageLine(model: *const Model, arena: std.mem.Allocator) []const u8 {
        return std.fmt.allocPrint(arena, "{s} bytes stored", .{
            formatCount(arena, model.bytes),
        }) catch "";
    }

    pub fn activityLine(model: *const Model, arena: std.mem.Allocator) []const u8 {
        return std.fmt.allocPrint(arena, "{s} active days / {s} seconds watched", .{
            formatCount(arena, model.active_days),
            formatCount(arena, model.activity_watched_seconds),
        }) catch "";
    }

    pub fn beginLoad(model: *Model) void {
        model.state = .loading;
        model.snapshot_ready = false;
        model.activity_ready = false;
        model.revision = 0;
        model.media_count = 0;
        model.course_count = 0;
        model.activity_count = 0;
        model.activity_watched_seconds = 0;
        model.active_days = 0;
        model.message_len = 0;
    }

    pub fn setFailure(model: *Model, message_value: []const u8) void {
        const value = if (message_value.len == 0 or
            message_value.len > max_message_bytes or
            !std.unicode.utf8ValidateSlice(message_value))
            "The learning ledger is unavailable."
        else
            message_value;
        @memcpy(model.message_storage[0..value.len], value);
        model.message_len = value.len;
        model.state = .failed;
        model.snapshot_request_id = 0;
        model.activity_request_id = 0;
        model.snapshot_ready = false;
        model.activity_ready = false;
        model.revision = 0;
        model.media_count = 0;
        model.course_count = 0;
        model.activity_count = 0;
        model.activity_watched_seconds = 0;
        model.active_days = 0;
    }

    pub fn load(model: *Model, bytes_value: []const u8, expected_revision: u64, expected_courses: u64) !void {
        const MediaPayload = struct {
            type: []const u8,
            lessons: u64,
            bytes: u64,
            completed: u64,
            watchedSeconds: u64,
        };
        const CoursePayload = struct {
            id: []const u8,
            name: []const u8,
            lessons: u64,
            completedLessons: u64,
            bytes: u64,
            watchedSeconds: u64,
        };
        const Payload = struct {
            revision: u64,
            totalCourses: u64,
            availableCourses: u64,
            missingCourses: u64,
            sections: u64,
            lessons: u64,
            completedLessons: u64,
            completionPercent: u32,
            bytes: u64,
            watchedSeconds: u64,
            totalSeconds: u64,
            mediaTypes: []const MediaPayload,
            topCourses: []const CoursePayload,
        };

        const parsed = try std.json.parseFromSlice(Payload, std.heap.page_allocator, bytes_value, .{});
        defer parsed.deinit();
        const stats = parsed.value;
        if (stats.completedLessons > stats.lessons) return error.InvalidLibraryStats;
        const expected_percent: u32 = if (stats.lessons == 0)
            0
        else
            @intCast((@as(u128, stats.completedLessons) * 100 + stats.lessons / 2) / stats.lessons);
        if (stats.revision == 0 or
            stats.revision != expected_revision or
            stats.totalCourses != expected_courses or
            stats.availableCourses > stats.totalCourses or
            stats.missingCourses != stats.totalCourses - stats.availableCourses or
            stats.completionPercent != expected_percent or
            stats.mediaTypes.len > max_media_rows or
            stats.topCourses.len > max_course_rows)
        {
            return error.InvalidLibraryStats;
        }

        var media_rows: [max_media_rows]MediaRow = undefined;
        var media_lessons: u128 = 0;
        var media_completed: u128 = 0;
        var media_bytes: u128 = 0;
        var media_watched_seconds: u128 = 0;
        var previous_media_rank: ?u3 = null;
        for (stats.mediaTypes, 0..) |source, index| {
            const rank = lessonKindRank(source.type) orelse return error.InvalidLibraryStats;
            if ((previous_media_rank != null and rank <= previous_media_rank.?) or
                source.completed > source.lessons)
            {
                return error.InvalidLibraryStats;
            }
            previous_media_rank = rank;
            for (media_rows[0..index]) |row| {
                if (std.mem.eql(u8, row.kind(), source.type)) return error.InvalidLibraryStats;
            }
            media_rows[index] = .{
                .kind_len = source.type.len,
                .lessons = source.lessons,
                .bytes = source.bytes,
                .completed = source.completed,
                .watched_seconds = source.watchedSeconds,
            };
            @memcpy(media_rows[index].kind_storage[0..source.type.len], source.type);
            media_lessons += source.lessons;
            media_completed += source.completed;
            media_bytes += source.bytes;
            media_watched_seconds += source.watchedSeconds;
        }
        if (media_lessons != stats.lessons or
            media_completed != stats.completedLessons or
            media_bytes != stats.bytes or
            media_watched_seconds != stats.watchedSeconds)
        {
            return error.InvalidLibraryStats;
        }

        var course_rows: [max_course_rows]CourseRow = undefined;
        for (stats.topCourses, 0..) |source, index| {
            if (!validRequiredText(source.id, max_course_id_bytes) or
                !validRequiredText(source.name, max_course_name_bytes) or
                source.completedLessons > source.lessons or
                source.lessons > stats.lessons or
                source.completedLessons > stats.completedLessons or
                source.bytes > stats.bytes or
                source.watchedSeconds > stats.watchedSeconds)
            {
                return error.InvalidLibraryStats;
            }
            if (index != 0 and !validTopCourseOrder(&course_rows[index - 1], source)) {
                return error.InvalidLibraryStats;
            }
            for (course_rows[0..index]) |row| {
                if (std.mem.eql(u8, row.id(), source.id)) return error.InvalidLibraryStats;
            }
            course_rows[index] = .{
                .id_len = source.id.len,
                .name_len = source.name.len,
                .lessons = source.lessons,
                .completed_lessons = source.completedLessons,
                .bytes = source.bytes,
                .watched_seconds = source.watchedSeconds,
            };
            @memcpy(course_rows[index].id_storage[0..source.id.len], source.id);
            @memcpy(course_rows[index].name_storage[0..source.name.len], source.name);
        }
        const expected_top_courses: usize = @intCast(@min(stats.totalCourses, max_course_rows));
        if (stats.topCourses.len != expected_top_courses) return error.InvalidLibraryStats;

        if (model.revision != 0 and model.revision != stats.revision) return error.InvalidLibraryStats;
        model.revision = stats.revision;
        model.total_courses = stats.totalCourses;
        model.available_courses = stats.availableCourses;
        model.missing_courses = stats.missingCourses;
        model.sections = stats.sections;
        model.lessons = stats.lessons;
        model.completed_lessons = stats.completedLessons;
        model.completion_percent = stats.completionPercent;
        model.bytes = stats.bytes;
        model.watched_seconds = stats.watchedSeconds;
        model.total_seconds = stats.totalSeconds;
        model.media_count = stats.mediaTypes.len;
        model.course_count = stats.topCourses.len;
        @memcpy(model.media_rows[0..stats.mediaTypes.len], media_rows[0..stats.mediaTypes.len]);
        @memcpy(model.course_rows[0..stats.topCourses.len], course_rows[0..stats.topCourses.len]);
        model.snapshot_ready = true;
        model.finishLoad();
    }

    pub fn loadActivity(model: *Model, bytes_value: []const u8, expected_revision: u64) !void {
        const RowPayload = struct {
            date: []const u8,
            watchedSeconds: u64,
            lessonsTouched: u64,
            completions: u64,
        };
        const Payload = struct {
            revision: u64,
            throughDate: []const u8,
            offset: u64,
            total: u64,
            rows: []const RowPayload,
        };

        const parsed = try std.json.parseFromSlice(Payload, std.heap.page_allocator, bytes_value, .{});
        defer parsed.deinit();
        const page = parsed.value;
        if (page.revision == 0 or
            page.revision != expected_revision or
            page.offset != 0 or
            page.total != page.rows.len or
            page.rows.len > activity_day_count)
        {
            return error.InvalidActivityPage;
        }

        const through_day = try parseEpochDay(page.throughDate);
        if (through_day < activity_day_count - 1) return error.InvalidActivityPage;
        const first_day = through_day - (activity_day_count - 1);

        var rows: [activity_day_count]ActivityRow = undefined;
        for (&rows, 0..) |*row, index| {
            const epoch_day = first_day + index;
            row.* = .{ .epoch_day = epoch_day };
            formatEpochDay(&row.date_storage, epoch_day);
        }

        var total_watched: u128 = 0;
        var max_watched: u64 = 1;
        var source_index: usize = 0;
        for (&rows) |*row| {
            if (source_index >= page.rows.len) continue;
            const source = page.rows[source_index];
            if (std.mem.eql(u8, source.date, row.date())) {
                row.watched_seconds = source.watchedSeconds;
                row.lessons_touched = source.lessonsTouched;
                row.completions = source.completions;
                total_watched += source.watchedSeconds;
                max_watched = @max(max_watched, source.watchedSeconds);
                source_index += 1;
            } else if (std.mem.order(u8, source.date, row.date()) == .lt) {
                return error.InvalidActivityPage;
            }
        }
        if (source_index != page.rows.len or total_watched > std.math.maxInt(u64)) {
            return error.InvalidActivityPage;
        }
        for (&rows) |*row| {
            if (row.watched_seconds == 0) {
                if (row.lessons_touched != 0 or row.completions != 0) row.level = 1;
                continue;
            }
            const numerator = @as(u128, row.watched_seconds) * 4;
            row.level = @intCast(@max(1, (numerator + max_watched - 1) / max_watched));
        }
        if (model.revision != 0 and model.revision != page.revision) return error.InvalidActivityPage;

        var cells: [activity_day_count]ActivityRow = undefined;
        for (rows, 0..) |row, index| {
            const visual_index = (index % 7) * 12 + index / 7;
            cells[visual_index] = row;
        }
        model.revision = page.revision;
        model.activity_rows = rows;
        model.activity_cells = cells;
        model.activity_count = activity_day_count;
        model.activity_watched_seconds = @intCast(total_watched);
        model.active_days = @intCast(page.rows.len);
        model.activity_ready = true;
        model.finishLoad();
    }

    fn finishLoad(model: *Model) void {
        if (!model.snapshot_ready or !model.activity_ready) return;
        model.state = .ready;
        model.message_len = 0;
    }
};

fn formatCount(arena: std.mem.Allocator, value: u64) []const u8 {
    var digits_buffer: [20]u8 = undefined;
    const digits = std.fmt.bufPrint(&digits_buffer, "{d}", .{value}) catch return "";
    const separator_count = (digits.len - 1) / 3;
    if (separator_count == 0) return arena.dupe(u8, digits) catch "";
    const formatted = arena.alloc(u8, digits.len + separator_count) catch return "";
    var source_index: usize = 0;
    var target_index: usize = 0;
    const first_group = if (digits.len % 3 == 0) 3 else digits.len % 3;
    while (source_index < digits.len) {
        if (source_index != 0 and source_index >= first_group and (source_index - first_group) % 3 == 0) {
            formatted[target_index] = ',';
            target_index += 1;
        }
        formatted[target_index] = digits[source_index];
        target_index += 1;
        source_index += 1;
    }
    return formatted;
}

fn lessonKindLabel(kind: []const u8) []const u8 {
    if (std.mem.eql(u8, kind, "video")) return "Video";
    if (std.mem.eql(u8, kind, "audio")) return "Audio";
    if (std.mem.eql(u8, kind, "document")) return "Document";
    if (std.mem.eql(u8, kind, "quiz")) return "Quiz";
    return "Lesson";
}

fn lessonKindRank(value: []const u8) ?u3 {
    if (std.mem.eql(u8, value, "video")) return 0;
    if (std.mem.eql(u8, value, "audio")) return 1;
    if (std.mem.eql(u8, value, "document")) return 2;
    if (std.mem.eql(u8, value, "quiz")) return 3;
    return null;
}

fn validLessonKind(value: []const u8) bool {
    return lessonKindRank(value) != null;
}

fn validTopCourseOrder(previous: *const CourseRow, current: anytype) bool {
    if (previous.watched_seconds != current.watchedSeconds) {
        return previous.watched_seconds > current.watchedSeconds;
    }
    if (previous.bytes != current.bytes) return previous.bytes > current.bytes;
    if (std.mem.eql(u8, previous.name(), current.name)) {
        return std.mem.order(u8, previous.id(), current.id) == .lt;
    }
    return true;
}

fn validRequiredText(value: []const u8, max_bytes: usize) bool {
    return value.len != 0 and
        value.len <= max_bytes and
        std.unicode.utf8ValidateSlice(value) and
        std.mem.indexOfScalar(u8, value, 0) == null;
}

fn parseEpochDay(value: []const u8) !u64 {
    if (value.len != 10 or value[4] != '-' or value[7] != '-') {
        return error.InvalidActivityPage;
    }
    const year = std.fmt.parseInt(std.time.epoch.Year, value[0..4], 10) catch
        return error.InvalidActivityPage;
    const month_number = std.fmt.parseInt(u4, value[5..7], 10) catch
        return error.InvalidActivityPage;
    const day = std.fmt.parseInt(u5, value[8..10], 10) catch
        return error.InvalidActivityPage;
    if (year < std.time.epoch.epoch_year or month_number < 1 or month_number > 12) {
        return error.InvalidActivityPage;
    }
    const month: std.time.epoch.Month = @enumFromInt(month_number);
    if (day == 0 or day > std.time.epoch.getDaysInMonth(year, month)) {
        return error.InvalidActivityPage;
    }

    var epoch_day: u64 = 0;
    var current_year: std.time.epoch.Year = std.time.epoch.epoch_year;
    while (current_year < year) : (current_year += 1) {
        epoch_day += std.time.epoch.getDaysInYear(current_year);
    }
    var current_month: std.time.epoch.Month = .jan;
    while (@intFromEnum(current_month) < month_number) {
        epoch_day += std.time.epoch.getDaysInMonth(year, current_month);
        current_month = @enumFromInt(@intFromEnum(current_month) + 1);
    }
    return epoch_day + day - 1;
}

fn formatEpochDay(buffer: *[10]u8, epoch_day: u64) void {
    const year_day = (std.time.epoch.EpochDay{ .day = @intCast(epoch_day) }).calculateYearDay();
    const month_day = year_day.calculateMonthDay();
    _ = std.fmt.bufPrint(buffer, "{d:0>4}-{d:0>2}-{d:0>2}", .{
        year_day.year,
        month_day.month.numeric(),
        month_day.day_index + 1,
    }) catch unreachable;
}

test "library stats reject impossible completion totals without trapping" {
    var model = Model{};
    try std.testing.expectError(
        error.InvalidLibraryStats,
        model.load(
            \\{"revision":7,"totalCourses":0,"availableCourses":0,"missingCourses":0,"sections":0,"lessons":1,"completedLessons":18446744073709551615,"completionPercent":0,"bytes":0,"watchedSeconds":0,"totalSeconds":0,"mediaTypes":[],"topCourses":[]}
        , 7, 0),
    );
}

test "library stats reject unknown fields and noncanonical row order" {
    const cases = [_][]const u8{
        \\{"revision":7,"totalCourses":0,"availableCourses":0,"missingCourses":0,"sections":0,"lessons":0,"completedLessons":0,"completionPercent":0,"bytes":0,"watchedSeconds":0,"totalSeconds":0,"mediaTypes":[],"topCourses":[],"extra":true}
        ,
        \\{"revision":7,"totalCourses":0,"availableCourses":0,"missingCourses":0,"sections":0,"lessons":2,"completedLessons":0,"completionPercent":0,"bytes":2,"watchedSeconds":0,"totalSeconds":0,"mediaTypes":[{"type":"document","lessons":1,"bytes":1,"completed":0,"watchedSeconds":0},{"type":"video","lessons":1,"bytes":1,"completed":0,"watchedSeconds":0}],"topCourses":[]}
        ,
        \\{"revision":7,"totalCourses":2,"availableCourses":2,"missingCourses":0,"sections":0,"lessons":2,"completedLessons":0,"completionPercent":0,"bytes":2,"watchedSeconds":3,"totalSeconds":0,"mediaTypes":[{"type":"video","lessons":2,"bytes":2,"completed":0,"watchedSeconds":3}],"topCourses":[{"id":"course-a","name":"A","lessons":1,"completedLessons":0,"bytes":1,"watchedSeconds":1},{"id":"course-b","name":"B","lessons":1,"completedLessons":0,"bytes":1,"watchedSeconds":2}]}
        ,
        \\{"revision":7,"totalCourses":2,"availableCourses":2,"missingCourses":0,"sections":0,"lessons":2,"completedLessons":0,"completionPercent":0,"bytes":2,"watchedSeconds":2,"totalSeconds":0,"mediaTypes":[{"type":"video","lessons":2,"bytes":2,"completed":0,"watchedSeconds":2}],"topCourses":[{"id":"course-z","name":"Same","lessons":1,"completedLessons":0,"bytes":1,"watchedSeconds":1},{"id":"course-a","name":"Same","lessons":1,"completedLessons":0,"bytes":1,"watchedSeconds":1}]}
        ,
    };
    for (cases) |payload| {
        var model = Model{};
        const expected_courses: u64 = if (std.mem.indexOf(u8, payload, "\"totalCourses\":2") == null) 0 else 2;
        model.load(payload, 7, expected_courses) catch continue;
        return error.TestExpectedError;
    }
}

test "activity pages are strict and zero fill the fixed 84 day window" {
    var model = Model{};
    try model.loadActivity(
        \\{"revision":7,"throughDate":"2026-07-23","offset":0,"total":4,"rows":[{"date":"2026-05-01","watchedSeconds":60,"lessonsTouched":1,"completions":0},{"date":"2026-05-02","watchedSeconds":0,"lessonsTouched":1,"completions":1},{"date":"2026-07-22","watchedSeconds":620,"lessonsTouched":2,"completions":1},{"date":"2026-07-23","watchedSeconds":30,"lessonsTouched":1,"completions":0}]}
    , 7);

    try std.testing.expectEqual(activity_day_count, model.activity_count);
    try std.testing.expectEqualStrings("2026-05-01", model.activity_rows[0].date());
    try std.testing.expectEqual(@as(u64, 60), model.activity_rows[0].watched_seconds);
    try std.testing.expectEqualStrings("2026-05-02", model.activity_rows[1].date());
    try std.testing.expectEqual(@as(u64, 0), model.activity_rows[1].watched_seconds);
    try std.testing.expectEqual(@as(u3, 1), model.activity_rows[1].level);
    try std.testing.expectEqualStrings("2026-07-23", model.activity_rows[83].date());
    try std.testing.expectEqual(@as(u64, 30), model.activity_rows[83].watched_seconds);
    try std.testing.expectEqual(@as(u64, 710), model.activity_watched_seconds);
    try std.testing.expectEqual(@as(u32, 4), model.active_days);
    try std.testing.expectEqual(@as(u3, 1), model.activity_rows[0].level);
    try std.testing.expectEqual(@as(u3, 4), model.activity_rows[82].level);
    try std.testing.expectEqualStrings("2026-05-02", model.activity_cells[12].date());
}

test "activity pages reject stale malformed and out of window rows" {
    const cases = [_][]const u8{
        \\{"revision":6,"throughDate":"2026-07-23","offset":0,"total":0,"rows":[]}
        ,
        \\{"revision":7,"throughDate":"2026-07-23","offset":1,"total":0,"rows":[]}
        ,
        \\{"revision":7,"throughDate":"2026-07-23","offset":0,"total":2,"rows":[{"date":"2026-07-22","watchedSeconds":1,"lessonsTouched":1,"completions":0}]}
        ,
        \\{"revision":7,"throughDate":"2026-07-23","offset":0,"total":1,"rows":[{"date":"2026-02-30","watchedSeconds":1,"lessonsTouched":1,"completions":0}]}
        ,
        \\{"revision":7,"throughDate":"2026-07-23","offset":0,"total":1,"rows":[{"date":"2026-04-30","watchedSeconds":1,"lessonsTouched":1,"completions":0}]}
        ,
        \\{"revision":7,"throughDate":"2026-07-23","offset":0,"total":2,"rows":[{"date":"2026-07-22","watchedSeconds":1,"lessonsTouched":1,"completions":0},{"date":"2026-07-22","watchedSeconds":2,"lessonsTouched":1,"completions":0}]}
        ,
        \\{"revision":7,"throughDate":"2026-02-30","offset":0,"total":0,"rows":[]}
        ,
    };

    for (cases) |payload| {
        var model = Model{};
        try std.testing.expectError(
            error.InvalidActivityPage,
            model.loadActivity(payload, 7),
        );
    }
}
