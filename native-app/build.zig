const std = @import("std");
const native_sdk = @import("native_sdk");

pub fn build(b: *std.Build) void {
    const artifacts = native_sdk.addAppArtifacts(b, b.dependency("native_sdk", .{}), .{ .name = "melearner" });
    const target = artifacts.exe.root_module.resolved_target.?.result;
    const host = b.graph.host.result;
    if (target.os.tag != .linux or host.os.tag != .linux or target.cpu.arch != host.cpu.arch or target.abi != host.abi) {
        @panic("the melearner native target currently links its Rust core only for the Linux host target");
    }

    const cargo = b.addSystemCommand(&.{"cargo"});
    cargo.addArgs(&.{
        "build",
        "--locked",
        "--release",
        "--manifest-path",
        "crates/melearner-core/Cargo.toml",
        "--target-dir",
    });
    cargo.setCwd(b.path(".."));
    const cargo_target = cargo.addOutputDirectoryArg("melearner-core-target");
    cargo.has_side_effects = true;

    const core_library = cargo_target.path(b, "release/libmelearner_core.a");
    for ([_]*std.Build.Module{
        artifacts.exe.root_module,
        artifacts.tests.root_module,
    }) |module| {
        module.addIncludePath(b.path("../include"));
        module.addObjectFile(core_library);
        inline for (.{ "c", "gcc_s", "util", "rt", "pthread", "m", "dl" }) |library| {
            module.linkSystemLibrary(library, .{});
        }
    }
}
