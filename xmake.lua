add_rules("mode.debug", "mode.release")

add_repositories("iceblcokmc https://github.com/IceBlcokMC/xmake-repo.git")

add_requires("nodejs 23.5.0") -- iceblockmc

local fmt_version = "fmt >=10.0.0 <11.0.0";
if is_plat("windows") then
    add_requires(fmt_version)
elseif is_plat("linux") then
    set_toolchains("clang")
    add_requires(fmt_version, {configs = {header_only = true}})
end

target("libnpm")
    set_license("GPL-3.0")
    set_kind("binary")
    add_defines(
        "NOMINMAX",
        "UNICODE",
        "_AMD64_"
    )
    add_files("src/**.cc")
    add_includedirs("src")
    add_packages("fmt", "nodejs")
    set_languages("cxx20")
    set_symbols("debug")

    if is_plat("windows") then
        add_cxxflags("/Zc:__cplusplus")
        add_cxflags(
            "/EHa",
            "/utf-8",
            "/sdl"
        )
    elseif is_plat("linux") then
        add_cxflags(
            "-fPIC",
            "-stdlib=libc++",
            "-fdeclspec",
            {force = true}
        )
        add_ldflags(
            "-stdlib=libc++",
            {force = true}
        )
        add_syslinks("dl", "pthread", "c++", "c++abi")
    end


