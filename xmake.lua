set_allowedplats("linux")

includes("config.lua")


local project_name = "coroutine"
local macro_prefix = "COROUTINE"
local version = "1.0.0"
local export_headers_module = "export-headers"
local export_headers_import_options = {rootdir = os.scriptdir(), anonymous = true}

-- 对外包依赖的唯一清单，每项填写 XMake 依赖规格字符串，例如 "fmt >=10.2.1"。common 对所有平台生效；windows 和 linux 分别只在对应目标平台生效。
-- 此表同时驱动 add_requires、target 的 add_packages 和发布 metadata；新增依赖只需在这里登记。
local package_dependencies = {
    common = {},
    windows = {},
    linux = {}
}


set_version(version)

set_xmakever("3.0.9")
set_project(project_name)
set_description("A Simple Coroutine Lib.")
set_languages("cxx17")

add_rules("mode.debug", "mode.release")

set_configvar("MACRO_PREFIX", macro_prefix)
set_configdir("$(builddir)/config/")
add_configfiles("src/config.h.in", "src/globalmacros.h.in")


-- 依赖表是对外包依赖的唯一声明源。配置当前平台时只启用 common 和当前平台的依赖；metadata 会记录整张表。
local active_package_dependencies = {}

local function enable_package_dependencies(requirements)
    for _, requirement in ipairs(requirements) do
        add_requires(requirement)
        table.insert(active_package_dependencies, requirement)
    end
end

local function package_name_from_spec(requirement)
    local name = requirement:match("^%s*([^%s<>=~!]+)")
    if not name then
        os.raise("invalid package requirement: %s", requirement)
    end
    return name
end

enable_package_dependencies(package_dependencies.common)

if is_plat("windows") then
    enable_package_dependencies(package_dependencies.windows)
elseif is_plat("linux") then
    enable_package_dependencies(package_dependencies.linux)
end


option("with_gtest")
    set_default(false)
    set_showmenu(true)
    set_description("Enable GoogleTest-based unit tests.")
option_end()

option("install_in_place")
    set_default(true)
    set_showmenu(true)
    set_description("Install to $(builddir)/$(plat)/$(arch)/$(mode)/install by default.")
option_end()

option("build_shared")
    set_default(default_build_shared_for_current_platform())
    set_showmenu(true)
    set_description("Build the " .. project_name .. " library as a shared library.")
option_end()


define_current_platform_options()

local build_shared = get_config("build_shared")

if build_shared == nil then
    build_shared = default_build_shared_for_current_platform()
end

local install_in_place = get_config("install_in_place")

if install_in_place == nil then
    install_in_place = true
end

if install_in_place then
    -- 请用 xmake install 安装。
    set_installdir("$(builddir)/$(plat)/$(arch)/$(mode)/install")
end


target(project_name)
    set_kind(build_shared and "shared" or "static")

    apply_current_platform_target_config()

    for _, requirement in ipairs(active_package_dependencies) do
        add_packages(package_name_from_spec(requirement), {public = true})
    end

    on_config(function (target)
        local json = import("core.base.json")
        local repository = os.getenv("GITHUB_REPOSITORY")
        local release_version = os.getenv("GITHUB_REF_NAME")

        local function dependency_specs(dependencies)
            local specs = {}

            for _, requirement in ipairs(dependencies) do
                table.insert(specs, requirement)
            end

            table.sort(specs)
            return json.mark_as_array(specs)
        end

        local manifest_path = path.join(os.projectdir(), "build", "package-metadata.json")
        os.mkdir(path.directory(manifest_path))

        io.writefile(manifest_path, json.encode({
            repo = repository,
            package_name = project_name,
            macro_prefix = macro_prefix,
            version = release_version,
            dependencies = {
                common = dependency_specs(package_dependencies.common),
                windows = dependency_specs(package_dependencies.windows),
                linux = dependency_specs(package_dependencies.linux)
            }
        }) .. "\n")
    end)

    if build_shared and is_current_win32() then
        -- <前缀>_BUILD_SHARED：使用动态库还是静态库。
        add_defines(macro_prefix .. "_BUILD_SHARED", {public = true})

        -- <前缀>_DLL_EXPORT：是否正在编译 DLL 本身。如果是，使用 __declspec(dllexport) 导出符号，否则是用户在使用 DLL 库，使用 __declspec(dllimport) 导入符号。
        add_defines(macro_prefix .. "_DLL_EXPORT")
    end

    set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/lib/")

    -- 会将 src 根目录和所有子目录一起匹配。
    add_files("src/**.cpp")

    add_includedirs("$(builddir)/config/", {public = true})
    add_includedirs("src", os.dirs("src/**"), {public = true})

    before_build(function (target)
        io.writefile(path.join(path.directory(target:targetdir()), ".version"), version)
    end)

    before_install(function (target)
        os.tryrm(target:installdir())
        os.mkdir(target:installdir())

        os.cp("$(builddir)/$(plat)/$(arch)/$(mode)/.version", target:installdir())

        local export_headers = import(export_headers_module, export_headers_import_options)
        export_headers.install_export_public_headers(target, target:installdir())
    end)

    before_package(function (target)
        os.tryrm(path.join(target:packagedir(), "$(plat)/$(arch)/$(mode)/"))
        os.mkdir(target:packagedir())

        os.cp("$(builddir)/$(plat)/$(arch)/$(mode)/.version", target:packagedir())
        os.cp("$(builddir)/$(plat)/$(arch)/$(mode)/.version", path.join(target:packagedir(), "$(plat)/$(arch)/$(mode)/.version"))

        local export_headers = import(export_headers_module, export_headers_import_options)
        export_headers.package_export_public_headers(target)
    end)
target_end()


includes("snippet")

if get_config("with_gtest") then
    includes("test")
end
