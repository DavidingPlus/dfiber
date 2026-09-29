function define_win32_options()
    option("suppress_w3_warnings")
        set_default(true)
        set_showmenu(true)
        set_description("Use /W2 instead of the default MSVC warning level.")
    option_end()
end

function default_win32_build_shared()
    return true
end

function apply_win32_target_config()
    -- /wd4251：导出包含 STL 成员的 C++ 类时，MSVC 会产生 C4251；当前项目采用类级 DLL 导出，该警告属于接口设计提示，暂时关闭，避免干扰正常构建输出。
    add_cxflags("/utf-8", "/wd4251")

    -- 只在 debug 模式下添加调试信息。
    if is_mode("debug") then
        set_symbols("debug", "embed")
    end

    -- XMake 默认也是这样设置的。
    -- if get_config("runtimes") == nil then
    --     if is_mode("debug") then
    --         set_runtimes("MDd")
    --     elseif is_mode("release") then
    --         set_runtimes("MD")
    --     end
    -- end

    if get_config("suppress_w3_warnings") ~= false then
        add_cxflags("/W2")
    end
end
