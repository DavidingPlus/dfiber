-- 这个脚本只服务于“发布阶段”的头文件导出。开发阶段仍然直接使用 src/ 下的原始头文件，不改源码里的 include 写法。到 install/package 时，再生成一份导出头文件树，并把其中的 quoted include 重写为稳定的发布路径，例如：#include "acceptor.h" 变成：#include "muduo-core/core/acceptor.h"
-- manifest 会在一次 xmake 运行里重复使用，所以做一层缓存，避免每个 hook 都重新扫描整个 src/**.h。
local public_header_manifest = nil

-- 统一把路径转成绝对路径并规范成 "/" 分隔，避免 Windows/Linux 路径分隔符不同导致查表失败。
local function normalize_path(filepath)
    return path.absolute(filepath):gsub("\\", "/")
end

-- 发布前缀直接取 target 名称，这样脚本不会硬编码死。如果以后 target 改名，导出路径会自动跟着变。
local function get_public_header_prefix(target)
    return target:name()
end

-- 配置头由 XMake 从 src/*.h.in 生成，与源码树中的 public header 不同，但发布时仍作为普通的 public header 一起进入同一份清单。
local function add_generated_public_header(manifest, public_header_prefix, filename)
    local entry = {
        relative = filename,
        source = "$(builddir)/config/" .. filename,
        install = (public_header_prefix .. "/" .. filename):gsub("\\", "/"),
        generated = true,
    }

    manifest.by_basename[filename] = entry
    table.insert(manifest.entries, entry)
end

-- 构造 public header 清单。这里记录三类索引：
-- 1. entries：顺序遍历用，导出时逐个生成文件。
-- 2. by_source：源码绝对路径 -> 导出条目。用于按“真正解析到的头文件”做精确映射。
-- 3. by_basename：文件名 -> 导出条目。只作为兜底手段；如果 basename 冲突，就标成 false。
local function get_public_header_manifest(target)
    if public_header_manifest then
        return public_header_manifest
    end

    local public_header_prefix = get_public_header_prefix(target)
    local manifest = {
        entries = {},
        by_source = {},
        by_basename = {},
    }

    -- 导出结构保留 src/ 下的相对层级。例如：
    -- src/core/tcpserver.h  -> muduo-core/core/tcpserver.h
    -- src/utils/callbacks.h -> muduo-core/utils/callbacks.h
    -- 这样发布目录和源码目录的语义保持一致，调试、排查和维护都更直接。
    for _, header in ipairs(os.files("src/**.h")) do
        local relative = path.relative(header, "src"):gsub("\\", "/")
        local source = normalize_path(header)
        local install = (public_header_prefix .. "/" .. relative):gsub("\\", "/")
        local entry = {
            relative = relative,
            source = source,
            install = install,
        }

        if manifest.by_source[source] then
            raise("duplicate public header source path: %s", source)
        end

        manifest.by_source[source] = entry
        table.insert(manifest.entries, entry)

        -- basename 兜底索引只在“文件名全局唯一”时才有效。一旦不同目录下出现同名头文件，例如：
        -- src/core/a.h
        -- src/utils/a.h
        -- 就不能再靠 basename 推断，否则会把 include 重写错。当然为了保持语义我绝大多数情况不会这样。
        local basename = path.filename(header)
        local existing = manifest.by_basename[basename]
        if existing == nil then
            manifest.by_basename[basename] = entry
        elseif existing ~= false then
            manifest.by_basename[basename] = false
        end
    end

    -- config.h 和 globalmacros.h 都由 XMake 生成，和 src/**.h 一样通过清单导出。
    add_generated_public_header(manifest, public_header_prefix, "config.h")
    add_generated_public_header(manifest, public_header_prefix, "globalmacros.h")

    public_header_manifest = manifest
    return manifest
end

-- 把源码里的 #include "xxx" 解析到“实际指向的 public header”。解析顺序尽量贴近编译器处理 quoted include 的直觉：
-- 1. 先看当前头文件所在目录。例如 src/core/tcpserver.h 中的 "acceptor.h" 应先命中 src/core/acceptor.h。
-- 2. 再看 src/ 根目录。例如 "globalmacros.h" 这种位于 src/ 根下的公共头。
-- 3. 最后才退化成 basename 查找，而且只在该 basename 唯一时允许。这个分支只是兼容一些历史写法，不应成为主路径。
local function resolve_public_header_entry(manifest, header, include_text)
    local current_dir_candidate = normalize_path(path.join(path.directory(header), include_text))
    if os.isfile(current_dir_candidate) then
        local entry = manifest.by_source[current_dir_candidate]
        if entry then
            return entry
        end
    end

    local source_root_candidate = normalize_path(path.join("src", include_text))
    if os.isfile(source_root_candidate) then
        local entry = manifest.by_source[source_root_candidate]
        if entry then
            return entry
        end
    end

    local basename = path.filename(include_text)
    local entry = manifest.by_basename[basename]
    if entry and entry ~= false then
        return entry
    end

    return nil
end

-- 只重写 quoted include，不碰系统头和第三方头。例如：#include <string>，#include <fmt/format.h> 这两类不应该被发布脚本介入。
local function rewrite_public_header_content(manifest, header, content)
    return (content:gsub('([ \t]*#include[ \t]+")([^"]+)(")', function(prefix, include_text, suffix)
        local entry = resolve_public_header_entry(manifest, header, include_text)
        if entry then
            return prefix .. entry.install .. suffix
        end

        -- 没命中的 include 原样保留。这意味着它要么不是 public header，要么本来就应该由外部 include path 解决。
        return prefix .. include_text .. suffix
    end))
end

-- 先导出到一个 staging 目录，再复制到 install/package。这样有两个好处：
-- 1. 源码头文件完全不被修改。
-- 2. install 和 package 可以复用同一套重写结果，逻辑集中，行为一致。
local function get_export_header_root(target)
    return path.join(path.directory(target:targetdir()), "export-headers")
end

-- 生成导出头文件树。产物形态大致如下：
--   export-headers/
--     include/
--       <target-name>/
--         core/...
--         utils/...
--         config.h
--         globalmacros.h
local function export_public_headers(target)
    local manifest = get_public_header_manifest(target)
    local public_header_prefix = get_public_header_prefix(target)
    local export_root = get_export_header_root(target)
    local export_include_root = path.join(export_root, "include", public_header_prefix)

    -- 每次导出都重建 staging 目录，避免残留旧文件影响结果。
    os.tryrm(export_root)
    os.mkdir(export_include_root)

    for _, entry in ipairs(manifest.entries) do
        local output = path.join(export_include_root, entry.relative)
        os.mkdir(path.directory(output))

        if entry.generated then
            os.cp(entry.source, output)
        else
            local rewritten = rewrite_public_header_content(manifest, entry.source, io.readfile(entry.source))
            io.writefile(output, rewritten)
        end
    end

    return {
        root = export_root,
        include_root = path.join(export_root, "include"),
        public_include_root = export_include_root,
    }
end

-- 安装阶段只负责把 staging 结果复制到最终安装目录的 include/ 下。install/include/<target-name>/... 是最终对外暴露的公共头结构。
function install_export_public_headers(target, installdir)
    local public_header_prefix = get_public_header_prefix(target)
    local exportinfo = export_public_headers(target)
    local include_root = path.join(installdir, "include")

    os.mkdir(include_root)
    os.tryrm(path.join(include_root, public_header_prefix))
    os.cp(exportinfo.public_include_root, include_root)
end

-- 打包阶段和安装阶段复用同一份 staging 逻辑，保证 install 与 package 的头文件内容完全一致，避免两套路径规则漂移。
function package_export_public_headers(target)
    local public_header_prefix = get_public_header_prefix(target)
    local exportinfo = export_public_headers(target)
    local include_root = path.join(target:packagedir(), "$(plat)/$(arch)/$(mode)/include")

    os.mkdir(include_root)
    os.tryrm(path.join(include_root, public_header_prefix))
    os.cp(exportinfo.public_include_root, include_root)
end
