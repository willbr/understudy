const std = @import("std");

const RAYLIB_VERSION = "5.5";
const RAYLIB_URL = "https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz";

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    // Default to an optimized build: stroke rasterization is several times
    // slower in Debug. Use -Doptimize=Debug when debugging.
    const optimize = b.option(
        std.builtin.OptimizeMode,
        "optimize",
        "Optimization mode (default: ReleaseFast)",
    ) orelse .ReleaseFast;

    const os_tag = target.result.os.tag;
    const is_windows = os_tag == .windows;
    const is_macos = os_tag == .macos;

    const raylib_root = b.pathFromRoot("deps/raylib-" ++ RAYLIB_VERSION);
    const raylib_header = b.pathJoin(&.{ raylib_root, "src", "raylib.h" });

    // Upstream raylib 5.5 has a build.zig that uses APIs removed in Zig 0.15
    // (ArrayList.init, defineCMacro). Bypass it entirely — fetch the source
    // tarball ourselves and compile the C files directly.
    ensureRaylib(b, raylib_header);

    const sqlite_dep = b.dependency("sqlite", .{});

    // ── raylib static lib ─────────────────────────────────────────────────────
    const raylib = b.addLibrary(.{
        .name = "raylib",
        .linkage = .static,
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });

    raylib.root_module.addCSourceFiles(.{
        .root = .{ .cwd_relative = raylib_root },
        .files = &.{
            "src/rcore.c",
            "src/rshapes.c",
            "src/rtextures.c",
            "src/rtext.c",
            "src/rmodels.c",
            "src/raudio.c",
            "src/utils.c",
            "src/rglfw.c",
        },
        .flags = &.{
            "-std=gnu99",
            "-D_GNU_SOURCE",
            "-DPLATFORM_DESKTOP",
            "-DPLATFORM_DESKTOP_GLFW",
            "-DGRAPHICS_API_OPENGL_33",
            "-Wno-missing-braces",
            "-Wno-unused-value",
            "-Wno-unused-parameter",
            "-Wno-unused-but-set-variable",
            "-Wno-implicit-function-declaration",
            "-fno-strict-aliasing",
        },
    });
    raylib.root_module.addIncludePath(.{ .cwd_relative = b.pathJoin(&.{ raylib_root, "src" }) });
    raylib.root_module.addIncludePath(.{ .cwd_relative = b.pathJoin(&.{ raylib_root, "src", "external" }) });
    raylib.root_module.addIncludePath(.{ .cwd_relative = b.pathJoin(&.{ raylib_root, "src", "external", "glfw", "include" }) });

    if (is_windows) {
        raylib.root_module.linkSystemLibrary("opengl32", .{});
        raylib.root_module.linkSystemLibrary("gdi32", .{});
        raylib.root_module.linkSystemLibrary("winmm", .{});
        raylib.root_module.linkSystemLibrary("shell32", .{});
        raylib.root_module.linkSystemLibrary("user32", .{});
    } else if (is_macos) {
        raylib.root_module.linkFramework("OpenGL", .{});
        raylib.root_module.linkFramework("Cocoa", .{});
        raylib.root_module.linkFramework("IOKit", .{});
        raylib.root_module.linkFramework("CoreAudio", .{});
        raylib.root_module.linkFramework("CoreVideo", .{});
    } else {
        raylib.root_module.linkSystemLibrary("GL", .{});
        raylib.root_module.linkSystemLibrary("m", .{});
        raylib.root_module.linkSystemLibrary("pthread", .{});
        raylib.root_module.linkSystemLibrary("dl", .{});
        raylib.root_module.linkSystemLibrary("rt", .{});
        raylib.root_module.linkSystemLibrary("X11", .{});
    }

    // ── sqlite3 static lib ────────────────────────────────────────────────────
    const sqlite = b.addLibrary(.{
        .name = "sqlite3",
        .linkage = .static,
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    sqlite.root_module.addCSourceFile(.{
        .file = sqlite_dep.path("sqlite3.c"),
        .flags = &.{
            "-DSQLITE_THREADSAFE=1",
            "-Wno-unused-but-set-variable",
            "-Wno-unused-parameter",
            "-Wno-unused-function",
        },
    });
    sqlite.root_module.addIncludePath(sqlite_dep.path(""));

    // ── understudy executable ─────────────────────────────────────────────────
    const exe = b.addExecutable(.{
        .name = "understudy",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });

    exe.root_module.addCSourceFiles(.{
        .files = &.{
            "src/main.c",
            "src/canvas.c",
            "src/toolbar.c",
            "src/tools.c",
            "src/ui.c",
            "src/db.c",
        },
        .flags = &.{
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Wno-unused-parameter",
            "-Wno-missing-field-initializers",
        },
    });

    if (is_macos) {
        exe.root_module.addCSourceFile(.{
            .file = b.path("src/clipboard_mac.m"),
            .flags = &.{ "-std=c11", "-Wall" },
        });
    } else if (is_windows) {
        exe.root_module.addCSourceFile(.{
            .file = b.path("src/clipboard_win.c"),
            .flags = &.{ "-std=c11", "-Wall" },
        });
    }

    exe.root_module.addIncludePath(.{ .cwd_relative = b.pathJoin(&.{ raylib_root, "src" }) });
    exe.root_module.addIncludePath(sqlite_dep.path(""));

    exe.root_module.linkLibrary(raylib);
    exe.root_module.linkLibrary(sqlite);

    if (is_windows) {
        exe.root_module.linkSystemLibrary("user32", .{});
        exe.root_module.linkSystemLibrary("gdi32", .{});
        exe.root_module.linkSystemLibrary("shell32", .{});
        exe.root_module.linkSystemLibrary("winmm", .{});
        exe.root_module.linkSystemLibrary("opengl32", .{});
        exe.subsystem = .Console;
    } else if (is_macos) {
        exe.root_module.linkFramework("Cocoa", .{});
        exe.root_module.linkFramework("IOKit", .{});
        exe.root_module.linkFramework("OpenGL", .{});
        exe.root_module.linkFramework("CoreVideo", .{});
        exe.root_module.linkFramework("CoreAudio", .{});
    }

    b.installArtifact(exe);

    const run_step = b.step("run", "Run Understudy");
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| run_cmd.addArgs(args);
    run_step.dependOn(&run_cmd.step);
}

// Zig 0.16 moved the filesystem API from std.fs to std.Io (every call takes an
// Io handle). These two helpers pick whichever exists so the script builds on
// both 0.15 and 0.16.
const old_fs = @hasDecl(std.fs, "accessAbsolute");

fn fileExists(b: *std.Build, abs_path: []const u8) bool {
    if (old_fs) {
        std.fs.accessAbsolute(abs_path, .{}) catch return false;
    } else {
        std.Io.Dir.accessAbsolute(b.graph.io, abs_path, .{}) catch return false;
    }
    return true;
}

fn deleteFile(b: *std.Build, abs_path: []const u8) void {
    if (old_fs) {
        std.fs.deleteFileAbsolute(abs_path) catch {};
    } else {
        std.Io.Dir.deleteFileAbsolute(b.graph.io, abs_path) catch {};
    }
}

fn ensureRaylib(b: *std.Build, marker: []const u8) void {
    if (fileExists(b, marker)) return;

    const deps = b.pathFromRoot("deps");
    const tarball = b.pathJoin(&.{ deps, "raylib-" ++ RAYLIB_VERSION ++ ".tar.gz" });

    std.debug.print("fetching raylib {s}\n", .{RAYLIB_VERSION});

    // b.run exits the build with the command's stderr if it fails.
    // curl's --create-dirs makes deps/ for us.
    _ = b.run(&.{ "curl", "-L", "--fail", "--silent", "--show-error", "--create-dirs", "-o", tarball, RAYLIB_URL });
    _ = b.run(&.{ "tar", "-xzf", tarball, "-C", deps });
    deleteFile(b, tarball);
}
