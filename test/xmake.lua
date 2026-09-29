if not get_config("with_gtest") then
    return
end

includes("config.lua")


add_requires("gtest")

local gtest_groups = {
    "dfiber",
    "testrun"
}

-- 请用 xmake test 执行测试。
target("tests")
    set_default(false)

    set_kind("binary")
    set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/test")
    add_deps("dfiber")
    add_packages("gtest")

    before_build(function ()
        os.tryrm("$(builddir)/$(plat)/$(arch)/$(mode)/test/gtest")
        os.mkdir("$(builddir)/$(plat)/$(arch)/$(mode)/test/gtest")
    end)

    on_install(function () end)
    apply_current_platform_target_config()

    add_files("main.cpp")
    apply_gtest_summary_config("tests", gtest_groups)

    for _, group in ipairs(gtest_groups) do
        add_tests(group, {
            realtime_output = true,
            files = {group .. "/*.cpp"}
        })
    end
target_end()
