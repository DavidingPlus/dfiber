includes("config-win32.lua", "config-linux.lua")


function is_current_win32()
    return is_plat("windows") or is_host("windows")
end

function is_current_linux()
    return is_plat("linux") or is_host("linux")
end

function define_current_platform_options()
    if is_current_win32() then
        define_win32_options()
        return
    end
    if is_current_linux() then
        define_linux_options()
        return
    end

    raise("Unknown platform")
end

function default_build_shared_for_current_platform()
    if is_current_win32() then
        return default_win32_build_shared()
    end
    if is_current_linux() then
        return default_linux_build_shared()
    end

    raise("Unknown platform")
end

function apply_current_platform_target_config()
    if is_current_win32() then
        apply_win32_target_config()
        return
    end
    if is_current_linux() then
        apply_linux_target_config()
        return
    end

    raise("Unknown platform")
end
