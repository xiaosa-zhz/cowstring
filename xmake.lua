add_rules("mode.debug", "mode.release")
add_files("src/*.cpp")
add_includedirs("includes")
set_languages("c++26")
set_encodings("utf-8")
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".vscode"})

-- Custom GCC (trunk) toolchain installed under ~/opt/gcc-trunk/.
local gcc_trunk_home = os.getenv("HOME") or os.getenv("USERPROFILE") or ""
local gcc_trunk_dir = path.join(gcc_trunk_home, "opt/gcc-trunk")

toolchain("gcc-trunk")
    set_kind("standalone")
    set_description("GCC trunk (~/opt/gcc-trunk)")
    set_sdkdir(gcc_trunk_dir)
    set_toolset("cc", path.join(gcc_trunk_dir, "bin/gcc"))
    set_toolset("cxx", path.join(gcc_trunk_dir, "bin/g++"), path.join(gcc_trunk_dir, "bin/gcc"))
    set_toolset("ld", path.join(gcc_trunk_dir, "bin/g++"), path.join(gcc_trunk_dir, "bin/gcc"))
    set_toolset("sh", path.join(gcc_trunk_dir, "bin/g++"), path.join(gcc_trunk_dir, "bin/gcc"))
    set_toolset("as", path.join(gcc_trunk_dir, "bin/gcc"))
    set_toolset("ar", path.join(gcc_trunk_dir, "bin/gcc-ar"))
    set_toolset("ranlib", path.join(gcc_trunk_dir, "bin/gcc-ranlib"))
    set_runtimes("stdc++_static", "stdc++_shared")
    on_load(function (toolchain)
        local march
        if toolchain:is_arch("x86_64", "x64") then
            march = "-m64"
        elseif toolchain:is_arch("i386", "x86") then
            march = "-m32"
        end
        if march then
            toolchain:add("cxflags", march)
            toolchain:add("mxflags", march)
            toolchain:add("asflags", march)
            toolchain:add("ldflags", march)
            toolchain:add("shflags", march)
        end
    end)

target("cowstring")
    set_kind("binary")
    set_toolchains("gcc-trunk")
    set_runtimes("stdc++_shared")
    add_cxxflags("-freflection", {force = true})
    add_linkdirs("/usr/local/lib/x86_64-unknown-linux-gnu")
    add_rpathdirs("/usr/local/lib/x86_64-unknown-linux-gnu")

-- target("llvm")
--     set_kind("binary")
--     set_toolchains("clang")
--     set_runtimes("c++_shared")
--     add_linkdirs("/usr/local/lib/x86_64-unknown-linux-gnu")
--     add_rpathdirs("/usr/local/lib/x86_64-unknown-linux-gnu")

-- target("gnu")
--     set_kind("binary")
--     set_toolchains("gcc")
--     set_runtimes("stdc++_shared")
--     add_linkdirs("/usr/local/lib/../lib64")
--     add_rpathdirs("/usr/local/lib/../lib64")
