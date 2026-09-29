local function _gtest_summary_outdir(target)
    -- 将 gtest 中间文件放到测试产物目录下，避免污染仓库根目录。
    return path.absolute(path.join(target:targetdir(), "gtest"), os.projectdir())
end

local function _gtest_summary_lockfile(target)
    return path.join(_gtest_summary_outdir(target), "gtest-summary.lock")
end

local function _gtest_summary_printedfile(target, session)
    return path.join(_gtest_summary_outdir(target), "gtest-summary." .. session .. ".printed")
end

local function _gtest_test_safe_name(name)
    return name:gsub("[/\\:]", "_")
end

local function _gtest_summary_resultfile(target, session, name)
    return path.join(_gtest_summary_outdir(target), "gtest-summary." .. session .. "." .. _gtest_test_safe_name(name) .. ".xml")
end

local function _gtest_summary_donefile(target, session, name)
    return path.join(_gtest_summary_outdir(target), "gtest-summary." .. session .. "." .. _gtest_test_safe_name(name) .. ".done")
end

local function _gtest_parse_result_xml(xml)
    xml = xml or ""
    -- 这里只取顶层 testsuites 汇总字段，不关心每个 testcase 的明细。
    local tests = tonumber(xml:match('<testsuites.- tests="(%d+)"')) or 0
    local failures = tonumber(xml:match('<testsuites.- failures="(%d+)"')) or 0
    local disabled = tonumber(xml:match('<testsuites.- disabled="(%d+)"')) or 0
    local skipped = tonumber(xml:match('<testsuites.- skipped="(%d+)"')) or 0
    local errors = tonumber(xml:match('<testsuites.- errors="(%d+)"')) or 0
    return {
        tests = tests,
        failures = failures,
        disabled = disabled,
        skipped = skipped,
        errors = errors,
        passed = math.max(0, tests - failures - disabled - skipped - errors)
    }
end

local function _gtest_print_summary(totals)
    local cyan = "\27[96m"
    local green = "\27[32m"
    local red = "\27[31m"
    local yellow = "\27[33m"
    local bright = "\27[1m"
    local reset = "\27[0m"

    print("")
    print(string.format(
        "%sgtest summary:%s %s%d%s passed, %s%d%s failed, %s%d%s disabled, %s%d%s total",
        bright .. cyan, reset,
        green, totals.passed or 0, reset,
        red, (totals.failures or 0) + (totals.errors or 0), reset,
        yellow, totals.disabled or 0, reset,
        bright, totals.tests or 0, reset
    ))
end

function apply_gtest_summary_config(target_name, groups)
    -- 同一次 xmake test 运行共用一个 session，用它把本轮生成的 xml/done 文件隔离开。
    local session = tostring(os.time()) .. "-" .. tostring(os.mclock())

    local function expected_test_names()
        local names = {}
        for _, group in ipairs(groups) do
            table.insert(names, target_name .. "/" .. group)
        end
        return names
    end

    local function ensure_outdir(target)
        return _gtest_summary_outdir(target)
    end

    before_test(function (target, opt)
        ensure_outdir(target)

        -- 多个 add_tests 会并行执行，这里用文件锁保护汇总状态文件的创建和读取。
        local lock = io.openlock(_gtest_summary_lockfile(target))
        assert(lock, "failed to open gtest summary lock")
        lock:lock()
        lock:unlock()
        lock:close()

        -- 让每个 add_tests 对应的 gtest 进程都输出一份独立 xml，后面再做聚合。
        local resultfile = _gtest_summary_resultfile(target, session, opt.name)
        opt._gtest_resultfile = resultfile

        local runargs = table.wrap(opt.runargs or target:get("runargs"))
        table.insert(runargs, "--gtest_output=xml:" .. resultfile)
        opt.runargs = runargs
    end)

    after_test(function (target, opt)
        ensure_outdir(target)

        local lock = io.openlock(_gtest_summary_lockfile(target))
        assert(lock, "failed to open gtest summary lock")
        lock:lock()

        -- 用 done 文件标记当前 add_tests 已经跑完，避免靠日志输出推断完成状态。
        io.writefile(_gtest_summary_donefile(target, session, opt.name), "")

        local completed = 0
        for _, name in ipairs(expected_test_names()) do
            if os.isfile(_gtest_summary_donefile(target, session, name)) then
                completed = completed + 1
            end
        end

        local printedfile = _gtest_summary_printedfile(target, session)
        -- 只在最后一个完成的 add_tests 上打印一次总汇总。
        local should_print = completed == #groups and not os.isfile(printedfile)
        if should_print then
            io.writefile(printedfile, "")
        end

        lock:unlock()
        lock:close()

        if should_print then
            local totals = {
                tests = 0,
                passed = 0,
                failures = 0,
                disabled = 0,
                skipped = 0,
                errors = 0
            }
            local missing = {}

            for _, name in ipairs(expected_test_names()) do
                local resultfile = _gtest_summary_resultfile(target, session, name)
                if os.isfile(resultfile) then
                    -- GTest 的 total 包含 disabled，所以 passed 需要从 total 中扣掉失败/禁用/跳过/错误。
                    local report = _gtest_parse_result_xml(io.readfile(resultfile))
                    for key, value in pairs(report) do
                        totals[key] = (totals[key] or 0) + value
                    end
                end
            end
            _gtest_print_summary(totals)
        end
    end)
end
