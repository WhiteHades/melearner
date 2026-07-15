const std = @import("std");
const builtin = @import("builtin");
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
const header_natural_height: f32 = 52;

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

pub const Model = struct {
    chrome_leading: f32 = 0,
    header_height: f32 = header_natural_height,
};

pub const Msg = union(enum) {
    chrome_changed: native_sdk.WindowChrome,

    pub const view_unbound = .{"chrome_changed"};
};

pub fn update(model: *Model, msg: Msg) void {
    switch (msg) {
        .chrome_changed => |chrome| {
            model.chrome_leading = chrome.insets.left;
            model.header_height = @max(header_natural_height, chrome.insets.top);
        },
    }
}

pub fn onChrome(chrome: native_sdk.WindowChrome) ?Msg {
    return .{ .chrome_changed = chrome };
}

pub const LibraryUi = canvas.Ui(Msg);
pub const library_markup = @embedFile("library.native");
pub const CompiledLibraryView = canvas.CompiledMarkupView(Model, Msg, library_markup);

const dev_markup_reload = builtin.mode == .Debug;
const LibraryApp = native_sdk.UiAppWithFeatures(Model, Msg, .{ .runtime_markup = dev_markup_reload });

pub fn main(init: std.process.Init) !void {
    const app_state = try std.heap.page_allocator.create(LibraryApp);
    defer std.heap.page_allocator.destroy(app_state);
    app_state.* = LibraryApp.init(std.heap.page_allocator, .{}, .{
        .name = "melearner",
        .scene = shell_scene,
        .canvas_label = canvas_label,
        .update = update,
        .on_chrome = onChrome,
        .view = CompiledLibraryView.build,
        .markup = if (dev_markup_reload)
            .{ .source = library_markup, .watch_path = "src/library.native", .io = init.io }
        else
            null,
    });
    defer app_state.deinit();

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
