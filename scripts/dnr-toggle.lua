-- dnr-toggle.lua — mpv 里给 dnr 插件做 A/B 开关与强度调节
--
-- 用法：
--   DNR_BYPASS_FILE=/tmp/dnr-bypass \
--   DNR_INTENSITY_FILE=/tmp/dnr-intensity \
--   mpv --script=scripts/dnr-toggle.lua --vf=vapoursynth=./dnr.vpy movie.mkv
--
-- 快捷键：
--   n        NR 开/关（通过创建/删除开关文件；插件每帧 stat 一次，立即生效，
--            不需要重建 filter、不会重新上传权重）
--   ] / [    强度 +0.1 / -0.1（写入 DNR_INTENSITY_FILE，插件每帧读取）
--
-- 原理：dnr 插件支持两个可选控制文件（见 src/vs_plugin.cpp）：
--   DNR_BYPASS_FILE    文件存在 → 当前帧直接透传原帧
--   DNR_INTENSITY_FILE 文件内容为浮点数 → 覆盖 intensity（0..2）
-- 这两条路径都不经过 VapourSynth 的图重建，所以切 A/B 是“瞬时”的。

local bypass_file = os.getenv("DNR_BYPASS_FILE") or "/tmp/dnr-bypass"
local intensity_file = os.getenv("DNR_INTENSITY_FILE") or "/tmp/dnr-intensity"
local intensity = tonumber(os.getenv("DNR_INTENSITY") or "1.0") or 1.0

local function file_exists(path)
    local f = io.open(path, "r")
    if f then
        f:close()
        return true
    end
    return false
end

local function write_all(path, text)
    local f = io.open(path, "w")
    if not f then
        mp.osd_message("DNR: 无法写入 " .. path .. "（检查目录权限）")
        return false
    end
    f:write(text)
    f:close()
    return true
end

local function toggle()
    if file_exists(bypass_file) then
        os.remove(bypass_file)
        mp.osd_message(string.format("DNR: ON  (intensity %.2f)", intensity))
    else
        write_all(bypass_file, "1\n")
        mp.osd_message("DNR: OFF (bypass)")
    end
end

local function adjust(delta)
    intensity = math.max(0.0, math.min(2.0, intensity + delta))
    if write_all(intensity_file, string.format("%.2f\n", intensity)) then
        mp.osd_message(string.format("DNR intensity: %.2f", intensity))
    end
end

-- 启动时默认开启 NR：清掉残留的开关文件，并写入初始强度
if file_exists(bypass_file) then
    os.remove(bypass_file)
end
write_all(intensity_file, string.format("%.2f\n", intensity))

mp.add_key_binding("n", "dnr-toggle", toggle)
mp.add_key_binding("]", "dnr-intensity-up", function() adjust(0.1) end)
mp.add_key_binding("[", "dnr-intensity-down", function() adjust(-0.1) end)

mp.msg.info(string.format(
    "dnr-toggle: n=开/关, [/]=强度; bypass=%s intensity=%s",
    bypass_file, intensity_file))
