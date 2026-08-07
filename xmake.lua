set_xmakever("3.0.0")

set_project("StarfieldAudioDuck")
set_version("0.1.1")
set_license("MIT")
set_arch("x64")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

includes("lib/commonlibsf")

target("StarfieldAudioDuck", function()
    add_rules("commonlibsf.plugin", {
        name = "Starfield Audio Duck",
        author = "Codex",
        description = "Mutes Starfield music while another Windows audio session is active.",
        email = ""
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    add_syslinks("ole32", "uuid")
    set_pcxxheader("src/pch.h")

    add_installfiles("res/StarfieldAudioDuck.ini", { prefixdir = "SFSE/Plugins" })
    add_installfiles("README.md", { prefixdir = "" })
end)
