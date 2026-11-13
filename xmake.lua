add_rules("mode.debug", "mode.release")
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
        local libdirs = {}
        for _, name in ipairs({"lib64", "lib"}) do
            local dir = path.join(toolchain:sdkdir(), name)
            if os.isdir(dir) then
                table.insert(libdirs, dir)
            end
        end
        toolchain:add("linkdirs",  libdirs)
        toolchain:add("rpathdirs", libdirs)
        toolchain:add("runenvs", "LD_LIBRARY_PATH", table.unpack(libdirs))

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

local function add_exp_feature_flags()
    add_cxxflags("-freflection", {force = true})
    add_cxxflags("-fcontracts", {force = true})
    add_cxxflags("-fcontract-evaluation-semantic=enforce", {force = true})
    add_ldflags("-fcontracts", {force = true})
end

-- Shared contract-violation handler used by all test binaries
target("contract")
    set_kind("object")
    set_default(false)
    set_toolchains("gcc-trunk")
    set_runtimes("stdc++_shared")
    add_files("src/contract.cpp")
    add_exp_feature_flags()

-- Regression test for self-referencing replace
target("test_replace_self")
    set_kind("binary")
    set_default(false)
    set_toolchains("gcc-trunk")
    set_runtimes("stdc++_shared")
    add_files("tests/replace_self.cpp")
    add_deps("contract")
    add_exp_feature_flags()
    add_tests("replace_self")

-- Internal COW state transitions tests
target("test_state")
    set_kind("binary")
    set_default(false)
    set_toolchains("gcc-trunk")
    set_runtimes("stdc++_shared")
    add_files("tests/state.cpp")
    add_deps("contract")
    add_exp_feature_flags()
    add_tests("state")

-- Conformance tests against std::string where the API semantics match
target("test_conformance")
    set_kind("binary")
    set_default(false)
    set_toolchains("gcc-trunk")
    set_runtimes("stdc++_shared")
    add_files("tests/conformance.cpp")
    add_deps("contract")
    add_exp_feature_flags()
    add_tests("conformance")

-- Compile-time conformance; in-place mutators are intentionally excluded
target("test_constexpr")
    set_kind("binary")
    set_default(false)
    set_toolchains("gcc-trunk")
    set_runtimes("stdc++_shared")
    add_files("tests/constexpr.cpp")
    add_deps("contract")
    add_exp_feature_flags()
    add_tests("constexpr")
