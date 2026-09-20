#pragma once
#include <windows.h>
#include <array>
#include <cwchar>
#include <string>

namespace taxi_camera::standalone {
// Companion display language. Bridge telemetry, logs and bug reports stay English.
enum class UiLanguage : UINT { follow_windows = 0, english = 1, simplified_chinese = 2 };
inline constexpr UINT kMaximumUiLanguage = static_cast<UINT>(UiLanguage::simplified_chinese);
inline constexpr wchar_t LanguageFile[] = L"settings.ini";
inline constexpr wchar_t LanguageSection[] = L"companion";
inline constexpr wchar_t LanguageKey[] = L"language";

inline UiLanguage language_preference = UiLanguage::follow_windows;
inline bool speaking_chinese = false;

// A locale name such as zh-CN or zh-Hans-HK. Traditional-only locales stay English.
inline bool locale_is_simplified_chinese(const wchar_t* name) {
  if (!name || _wcsnicmp(name, L"zh", 2) != 0)
    return false;
  const wchar_t* suffix = name + 2;
  if (!suffix[0])
    return true;
  return wcsstr(suffix, L"Hans") || wcsstr(suffix, L"hans") || _wcsnicmp(suffix, L"-CN", 3) == 0 || _wcsnicmp(suffix, L"-SG", 3) == 0;
}

inline std::wstring ui_locale_name() {
  wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
  if (!GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH))
    return {};
  return name;
}

inline void apply_language(UiLanguage preference, const std::wstring& locale_name) {
  language_preference = preference;
  speaking_chinese = preference == UiLanguage::simplified_chinese ||
                     (preference == UiLanguage::follow_windows && locale_is_simplified_chinese(locale_name.c_str()));
}

inline UiLanguage load_language(const std::wstring& directory) {
  if (directory.empty())
    return UiLanguage::follow_windows;
  const auto value = GetPrivateProfileIntW(LanguageSection, LanguageKey, static_cast<int>(UiLanguage::follow_windows),
                                           (directory + L"\\" + LanguageFile).c_str());
  if (value < 0 || static_cast<UINT>(value) > kMaximumUiLanguage)
    return UiLanguage::follow_windows;
  return static_cast<UiLanguage>(value);
}

inline bool save_language(const std::wstring& directory, UiLanguage language) {
  if (directory.empty() || static_cast<UINT>(language) > kMaximumUiLanguage)
    return false;
  wchar_t value[8];
  std::swprintf(value, 8, L"%u", static_cast<UINT>(language));
  return WritePrivateProfileStringW(LanguageSection, LanguageKey, value, (directory + L"\\" + LanguageFile).c_str()) != FALSE;
}

struct Message {
  const wchar_t* english;
  const wchar_t* chinese;
};

inline constexpr Message kChineseCatalog[]{
    // Sidebar, page titles and shared controls.
    {L"Overview", L"概览"},
    {L"Camera views", L"摄像头视角"},
    {L"Display", L"显示"},
    {L"PFD routing", L"PFD 路由"},
    {L"Diagnostics", L"诊断"},
    {L"Reference guides", L"参考标线"},
    {L"Menu", L"菜单"},
    {L"Save changes", L"保存更改"},
    {L"Hide to tray", L"隐藏到托盘"},
    {L"Donate", L"捐赠"},
    {L"Report a bug", L"报告问题"},
    {L"Settings", L"设置"},
    {L"Exit", L"退出"},
    {L"Native taxi cameras", L"原生滑行摄像头"},
    {L"WINDOWS COMPANION", L"WINDOWS 伴随程序"},
    {L"Taxi camera", L"滑行摄像头"},
    {L"Your taxi cameras, controlled from the flight deck.", L"在驾驶舱内操控你的滑行摄像头。"},
    {L"Fine-tune each camera independently. Changes stay with this aircraft.", L"分别微调每路摄像头。更改只作用于当前机型。"},
    {L"Balance visibility, colour and camera update rate.", L"在可见度、颜色与摄像头刷新率之间取得平衡。"},
    {L"Connect each TAXI button to the correct display.", L"将每个 TAXI（滑行）按钮连接到正确的显示屏。"},
    {L"Live status and the controls used during camera testing.", L"实时状态，以及摄像头测试期间使用的控制项。"},
    {L"Move the guide points, preview them live, then save for this aircraft.", L"移动各标线位置，实时预览，然后为当前机型保存。"},
    {L"Donate via PayPal", L"通过 PayPal 捐赠"},
    {L"Open Taxi Cam on GitHub", L"在 GitHub 打开 Taxi Cam"},
    {L": On", L"：开"},
    {L": Off", L"：关"},
    {L"On", L"开"},
    {L"Off", L"关"},
    {L"on", L"开"},
    {L"off", L"关"},
    {L"Connect", L"连接"},
    {L"Disconnect", L"断开"},
    // Overview page.
    {L"Aircraft profile", L"机型配置"},
    {L"Auto aircraft", L"自动识别机型"},
    {L"Auto-connect", L"自动连接"},
    {L"TAXI buttons", L"TAXI 按钮"},
    {L"TAXI buttons: On", L"TAXI 按钮：开"},
    {L"TAXI buttons: Off", L"TAXI 按钮：关"},
    {L"Keyboard shortcuts…", L"键盘快捷键…"},
    {L"Flight-deck control", L"驾驶舱控制"},
    {L"Left and right EFIS TAXI buttons activate their own PFD.", L"左右两侧 EFIS 上的 TAXI 按钮分别激活各自的 PFD。"},
    {L"Use Keyboard shortcuts or PFD routing previews for this aircraft.", L"该机型请使用「键盘快捷键」或 PFD 路由预览。"},
    {L"Camera frame rate", L"摄像头帧率"},
    {L"Min 5 fps per camera (range 5–60). This install sets 10.", L"每路摄像头最低 5 帧/秒（范围 5–60）。本次安装默认设为 10。"},
    {L"Cameras and TAXI buttons turn off above 60 knots.", L"速度超过 60 节后，摄像头画面与 TAXI 按钮会关闭。"},
    {L"Waiting for the simulator", L"等待模拟器"},
    {L"Native bridge connected", L"原生桥接已连接"},
    {L"Native bridge connected — waiting for cockpit displays", L"原生桥接已连接 — 等待驾驶舱显示屏"},
    {L"Disconnected", L"已断开"},
    {L"Waiting for cockpit displays to be drawn. "
     L"Restart Flight only if the list stays empty.",
     L"正在等待驾驶舱显示屏绘制完成。仅当列表始终为空时才重新开始航班。"},
    {L"Changes are saved for this aircraft.", L"更改会保存到当前机型。"},
    {L"Unsaved changes", L"有未保存的更改"},
    {L"Waiting for Microsoft Flight Simulator 2024", L"正在等待 Microsoft Flight Simulator 2024"},
    {L"MSFS detected. Auto-connect is off — choose Connect when ready.", L"已检测到 MSFS。自动连接已关闭 — 准备好后请选择「连接」。"},
    {L"Auto-connect on. Taxi Cam will connect when the simulator is available.", L"自动连接已开启。模拟器可用时 Taxi Cam 会自行连接。"},
    {L"Auto-connect off. Use Connect whenever you are ready, including in a loaded flight.",
     L"自动连接已关闭。准备好后随时可以选择「连接」，已进入航班时同样适用。"},
    {L"Could not save the Auto-connect preference.", L"无法保存自动连接偏好。"},
    {L"Could not remember the first launch. Settings may reopen next time.", L"无法记录首次启动状态。下次可能仍会打开设置窗口。"},
    {L"Saved settings were invalid; profile defaults loaded.", L"已保存的设置无效，已改用机型默认值。"},
    {L"Aircraft profile selected. Reconnecting its cameras and displays.", L"已选择机型配置。正在重新连接其摄像头与显示屏。"},
    {L"Could not load that aircraft profile.", L"无法载入该机型配置。"},
    {L"Could not apply that aircraft profile.", L"无法应用该机型配置。"},
    {L"Aircraft detected. Its saved calibration is active.", L"已识别机型。该机型保存的校准已生效。"},
    {L"Could not save settings. Check access to your local settings folder.", L"无法保存设置。请检查本地设置文件夹的访问权限。"},
    {L"Saved. Adjustments apply while the cameras are running.", L"已保存。调整将在摄像头运行期间生效。"},
    {L"Settings updated.", L"设置已更新。"},
    {L"Finish the current values before changing pages.", L"请先完成当前数值的填写，再切换页面。"},
    {L"Check the values: rate 5–60 (min 5), EV −16 to +4, lens 0.05–1.55.",
     L"请检查数值：帧率 5–60（最低 5）、EV −16 至 +4、视场角 0.05–1.55。"},
    {L"Saved. Some shortcuts are unavailable; check Overview > Flight-deck control.",
     L"已保存。部分快捷键不可用，请检查「概览 > 驾驶舱控制」。"},
    {L"Some shortcuts are unavailable. Check Overview > Flight-deck control.", L"部分快捷键不可用。请检查「概览 > 驾驶舱控制」。"},
    // Camera views page.
    {L"Nose-wheel camera", L"前轮摄像头"},
    {L"Tail camera", L"尾部摄像头"},
    {L"UPPER VIEW", L"上方视角"},
    {L"LOWER VIEW", L"下方视角"},
    {L"Right (m)", L"右移（米）"},
    {L"Up (m)", L"上移（米）"},
    {L"Forward (m)", L"前移（米）"},
    {L"Pitch (deg)", L"俯仰（度）"},
    {L"Yaw (deg)", L"偏航（度）"},
    {L"Lens (rad)", L"视场角（弧度）"},
    {L"Position relative to the aircraft datum", L"相对飞机基准点的位置"},
    {L"Lower 0.25 m", L"降低 0.25 米"},
    {L"Raise 0.25 m", L"升高 0.25 米"},
    {L"Aft 1 m", L"后移 1 米"},
    {L"Forward 1 m", L"前移 1 米"},
    {L"Reset camera mounts", L"重置摄像头位置"},
    {L"Positive pitch looks up. Positive yaw looks right.", L"俯仰为正时朝上，偏航为正时朝右。"},
    // Display page.
    {L"Daytime exposure", L"昼间曝光"},
    {L"Automatic night exposure", L"夜间自动曝光"},
    {L"Maximum night boost", L"夜间最大增益"},
    {L"Exposure compensation in EV. Your calibrated baseline is −8.8.", L"以 EV 为单位的曝光补偿。你的校准基线为 −8.8。"},
    {L"Gradually brighten the camera display as ambient light drops.", L"环境光变暗时，逐步提亮摄像头画面。"},
    {L"Additional exposure at night, from 0 to +8 EV. Default: +8 EV.", L"夜间额外曝光，范围 0 至 +8 EV。默认：+8 EV。"},
    {L"Activation limit per camera: min 5 fps, range 5–60. Install default: 10.",
     L"每路摄像头的激活上限：最低 5 帧/秒，范围 5–60。安装默认：10。"},
    {L"Ground-speed colour", L"地速颜色"},
    {L"Currently applied exposure: %.2f EV", L"当前已应用的曝光：%.2f EV"},
    {L"Applied exposure appears when the camera bridge connects.", L"摄像头桥接连接后，这里会显示已应用的曝光。"},
    {L"Auto exposure", L"自动曝光"},
    // PFD routing page.
    {L"PFD assignment", L"PFD 分配"},
    {L"Target identities apply to this simulator session.", L"目标标识只适用于当前模拟器会话。"},
    {L"LEFT PFD", L"左侧 PFD"},
    {L"RIGHT PFD", L"右侧 PFD"},
    {L"Auto detect", L"自动检测"},
    {L"Automatic assignment", L"自动分配"},
    {L"Refresh textures", L"刷新纹理"},
    {L"Swap left / right", L"左右互换"},
    {L"Manual camera preview", L"手动摄像头预览"},
    {L"Manual preview and calibration turn off automatic TAXI-button control.", L"手动预览与校准会关闭 TAXI 按钮的自动控制。"},
    {L"Target calibration", L"目标校准"},
    {L"Animated bars identify each screen before enabling a live feed.", L"在启用实时画面前，先用动态条纹辨认各个屏幕。"},
    {L"Left preview", L"左侧预览"},
    {L"Right preview", L"右侧预览"},
    {L"Calibrate left", L"校准左侧"},
    {L"Calibrate right", L"校准右侧"},
    {L"Left preview: Off", L"左侧预览：关"},
    {L"Right preview: Off", L"右侧预览：关"},
    {L"Calibrate left: Off", L"校准左侧：关"},
    {L"Calibrate right: Off", L"校准右侧：关"},
    {L"Left preview: ", L"左侧预览："},
    {L"Right preview: ", L"右侧预览："},
    {L"Use shortcuts in Overview > Flight-deck control, or manual previews.", L"请改用「概览 > 驾驶舱控制」中的快捷键，或手动预览。"},
    {L"Enable flight-deck control on Overview to return to normal use.", L"在「概览」页启用驾驶舱控制即可恢复正常使用。"},
    {L"Choose different textures for left and right, or Automatic assignment.", L"请为左右两侧选择不同的纹理，或选择「自动分配」。"},
    {L"Display assignment request limit reached. Restart Taxi Cam.", L"显示屏分配请求已达上限。请重新启动 Taxi Cam。"},
    {L"#%llu | %ux%u | %u mips | format %u", L"#%llu | %ux%u | %u 级 mip | 格式 %u"},
    // Diagnostics page.
    {L"Bridge                 %s\nCamera pair         %s\nLeft / right PFD    %llu / %llu\nCaptured frames  "
     L"%llu\nCompositions       %llu\nPFD writes            %llu\nHook failures        %llu",
     L"桥接程序：%s\n摄像头组：%s\n左 / 右 PFD：%llu / %llu\n采集帧数：%llu\n合成次数：%llu\nPFD 写入：%llu\n钩子失败：%llu"},
    {L"Probe CPU: %.2f ms (max %.2f)\nManager %.3f   Pool %.3f\nLifecycle %.3f   Entries %.3f\nView 1 %.3f   View 2 "
     L"%.3f\nHandoff %.3f   Pose %.3f\nActivation %.3f   Publish %.3f\n\nExcludes engine rendering and GPU time.",
     L"探针 CPU：%.2f 毫秒（最大 %.2f）\n管理器 %.3f   资源池 %.3f\n生命周期 %.3f   条目 %.3f\n视角 1 %.3f   视角 2 "
     L"%.3f\n交接 %.3f   姿态 %.3f\n激活 %.3f   发布 %.3f\n\n不含引擎渲染与 GPU 时间。"},
    {L"Connected", L"已连接"},
    {L"Waiting for displays", L"等待显示屏"},
    {L"Waiting", L"等待中"},
    {L"Ready", L"就绪"},
    {L"Scene test renders without PFD delivery.", L"场景测试只渲染画面，不输出到 PFD。"},
    {L"Calibration batches per 50 ms (64–16384)", L"每 50 毫秒的校准批次（64–16384）"},
    {L"Scene test", L"场景测试"},
    {L"Scene test: Off", L"场景测试：关"},
    {L"First camera only", L"仅第一路摄像头"},
    {L"Open log folder", L"打开日志文件夹"},
    {L"Stop camera tests", L"停止摄像头测试"},
    // Reference guides page.
    {L"Nose-wheel view", L"前轮视角"},
    {L"Tail view", L"尾部视角"},
    {L"Nose squares", L"机头方块"},
    {L"Nose dot", L"机头圆点"},
    {L"Upper endpoint", L"上端点"},
    {L"Outside corner", L"外侧角点"},
    {L"Inner endpoint", L"内侧端点"},
    {L"X from left (%)", L"距左侧（%）"},
    {L"Y from top (%)", L"距顶部（%）"},
    {L"X: 0–50%. Y: 0–100% of each camera view. The right guide mirrors the left.",
     L"X 为各摄像头画面的 0–50%，Y 为 0–100%。右侧标线是左侧的镜像。"},
    {L"Preview is temporary until saved. Reset restores guide positions and colour.",
     L"保存之前的预览只是临时效果。「重置标线」可恢复标线位置与颜色。"},
    {L"Mirrored preview", L"镜像预览"},
    {L"Apply live", L"实时应用"},
    {L"Reset guides", L"重置标线"},
    {L"Marking colour", L"标线颜色"},
    {L"Preview applied. Save changes to keep these guides.", L"已应用预览。请保存更改以保留这些标线。"},
    {L"Profile guide positions and colour restored. Save changes to keep them.", L"已恢复该机的标线位置与颜色。请保存更改以保留。"},
    {L"Guide X must be 0–50%; Y must be 0–100%. Enter finite numbers.", L"标线 X 必须为 0–50%，Y 必须为 0–100%。请填写有效数值。"},
    // Keyboard shortcut editor.
    {L"Taxi Cam — Flight-deck keyboard shortcuts", L"Taxi Cam — 驾驶舱键盘快捷键"},
    {L"Flight-deck keyboard shortcuts", L"驾驶舱键盘快捷键"},
    {L"Use Ctrl or Alt with a letter, number or function key. Clear disables a shortcut.",
     L"可使用 Ctrl 或 Alt 配合字母、数字或功能键。点击「清除」可禁用某个快捷键。"},
    {L"Clear", L"清除"},
    {L"Both turns both displays on; press again to turn both off.\nShortcuts apply to all aircraft and work while Taxi Cam is hidden.",
     L"「两个摄像头」会同时点亮两块显示屏，再次按下则同时关闭。\n快捷键对所有机型有效，并在 Taxi Cam 隐藏时依然可用。"},
    {L"Reset shortcuts", L"重置快捷键"},
    {L"Close", L"关闭"},
    {L"Left camera", L"左侧摄像头"},
    {L"Right camera", L"右侧摄像头"},
    {L"Both cameras", L"两个摄像头"},
    {L"Unsaved — select Save changes to apply", L"未保存 — 请选择「保存更改」后生效"},
    {L"Editing — shortcuts paused until you leave the field", L"正在编辑 — 离开输入框之前快捷键暂停"},
    {L"Disabled", L"已禁用"},
    {L"Preview only — shortcut not registered", L"仅预览 — 快捷键未注册"},
    {L"Ready — works while Taxi Cam is hidden", L"就绪 — Taxi Cam 隐藏时仍可使用"},
    {L"Unavailable — another app uses this shortcut", L"不可用 — 其他程序占用了该快捷键"},
    {L"Unavailable — Windows error ", L"不可用 — Windows 错误 "},
    {L"Use Ctrl or Alt with a letter, number or function key (except F12), or clear the shortcut.",
     L"请使用 Ctrl 或 Alt 配合字母、数字或功能键（F12 除外），或清除该快捷键。"},
    {L"Each camera action needs a different shortcut. Clear any shortcut you do not need.",
     L"每个摄像头操作需要使用不同的快捷键。请清除不需要的快捷键。"},
    {L"Could not open the keyboard shortcut editor.", L"无法打开键盘快捷键编辑器。"},
    {L"Could not save shortcuts. Check access to the local settings folder.", L"无法保存快捷键。请检查本地设置文件夹的访问权限。"},
    {L"Saved. Unavailable shortcuts need a different combination. The other shortcuts remain active.",
     L"已保存。不可用的快捷键需要换一组组合，其余快捷键仍然有效。"},
    {L"Shortcuts saved for all aircraft. Camera settings and unfinished edits are unchanged.",
     L"已为所有机型保存快捷键。摄像头设置与未完成的编辑保持不变。"},
    {L"Saved shortcuts were invalid and disabled. Configure them in Overview > Flight-deck control.",
     L"已保存的快捷键无效并已禁用。请在「概览 > 驾驶舱控制」中重新设置。"},
    // Connection and camera state.
    {L"Choose Connect before using aircraft camera shortcuts.", L"请先选择「连接」，再使用机型摄像头快捷键。"},
    {L"Waiting for current aircraft TAXI-button state. Try the shortcut again when connected.",
     L"正在等待当前机型的 TAXI 按钮状态。连接完成后请再次尝试快捷键。"},
    {L"Camera request: left ", L"摄像头请求：左侧 "},
    {L", right ", L"，右侧 "},
    {L".", L"。"},
    {L"Camera request updated. Choose Connect to enable camera output.", L"摄像头请求已更新。请选择「连接」以启用摄像头画面输出。"},
    {L"Disconnected. Camera output and temporary requests are off.", L"已断开。摄像头输出与临时请求均已关闭。"},
    {L"Connect requested. Cameras will enable when the bridge is ready.", L"已请求连接。桥接就绪后摄像头将启用。"},
    {L"Disconnected. Choose Connect to enable the cameras again.", L"已断开。选择「连接」可重新启用摄像头。"},
    {L"Restart Taxi Cam to begin a new connection.", L"请重新启动 Taxi Cam 以建立新的连接。"},
    {L"Connect requested. Retrying bridge attach.", L"已请求连接。正在重试附加桥接。"},
    {L"Connect requested. Waiting for Microsoft Flight Simulator 2024.", L"已请求连接。正在等待 Microsoft Flight Simulator 2024。"},
    {L"Simulator closed. Waiting for the next session.", L"模拟器已关闭。正在等待下一次会话。"},
    {L"Could not open the camera control channel.", L"无法打开摄像头控制通道。"},
    {L" Retrying.", L"正在重试。"},
    {L" Choose Disconnect, then Connect to try again.", L"请先选择「断开」，再选择「连接」重试。"},
    {L" Automatic recovery scheduled.", L"已安排自动恢复。"},
    {L" Retrying startup preflight.", L"正在重试启动预检。"},
    {L" Automatic retries paused; choose Disconnect, then Connect to retry.", L"自动重试已暂停；请先选择「断开」，再选择「连接」重试。"},
    {L" (Windows ", L"（Windows "},
    {L")", L"）"},
    {L"Bridge status went stale. Retrying attach automatically.", L"桥接状态已过期。正在自动重试附加。"},
    // Launcher and bridge diagnostics; the same wording stays English in launcher.log.
    {L"Native bridge loaded. Waiting for graphics and aircraft.", L"原生桥接已载入。正在等待图形设备与机型信息。"},
    {L"Cannot open MSFS in this Windows session.", L"无法在当前 Windows 会话中打开 MSFS。"},
    {L"MSFS must run under the same Windows account.", L"MSFS 必须与 Taxi Cam 运行在同一 Windows 账户下。"},
    {L"Simulator executable path changed; attach refused.", L"模拟器可执行文件路径已更改，已拒绝附加。"},
    {L"Cannot read the simulator module list during startup.", L"启动期间无法读取模拟器模块列表。"},
    {L"The legacy taxi add-on is loaded. Run the native installer, then restart MSFS.",
     L"检测到旧版滑行插件已载入。请运行原生安装程序，然后重新启动 MSFS。"},
    {L"Waiting for readable, valid MSFS executable headers.", L"正在等待可读且有效的 MSFS 可执行文件头。"},
    {L"Waiting for the previous bridge load to finish; no second Windows load will be started.",
     L"正在等待上一次桥接载入完成；不会启动第二次 Windows 载入。"},
    {L"Waiting for the verified Windows DLL loader in MSFS.", L"正在等待 MSFS 中经过验证的 Windows DLL 加载器。"},
    {L"Could not allocate the bridge path.", L"无法为桥接路径分配内存。"},
    {L"Could not write the bridge path.", L"无法写入桥接路径。"},
    {L"Windows refused to load the camera bridge.", L"Windows 拒绝加载摄像头桥接。"},
    {L"Windows finished the load attempt, but the bridge module check failed. See launcher.log.",
     L"Windows 已完成加载尝试，但桥接模块校验失败。请查看 launcher.log。"},
    {L"The bridge was not found after Windows loading completed. See launcher.log for details.",
     L"Windows 加载完成后未找到桥接模块。详情请查看 launcher.log。"},
    {L"Cannot read the bridge's exported entry point.", L"无法读取桥接导出的入口点。"},
    {L"Invalid native bridge entry point.", L"原生桥接入口点无效。"},
    {L"Cannot start the loaded bridge.", L"无法启动已加载的桥接。"},
    {L"Cannot read the native bridge startup result. See launcher.log.", L"无法读取原生桥接的启动结果。请查看 launcher.log。"},
    {L"The native bridge refused startup.", L"原生桥接拒绝启动。"},
    {L"Stopped waiting for native bridge startup; startup may still finish.", L"已停止等待原生桥接启动；启动仍可能完成。"},
    {L"Stopped waiting for bridge loading; the Windows load may still finish.", L"已停止等待桥接加载；Windows 加载仍可能完成。"},
    {L"Native bridge startup is pending.", L"原生桥接启动尚待完成。"},
    {L"Bridge loading is still pending; no second load will be attempted this session.", L"桥接加载仍在进行；本次会话不会尝试第二次加载。"},
    {L"Windows could not wait for native bridge startup. See launcher.log.", L"Windows 无法等待原生桥接启动。请查看 launcher.log。"},
    {L"Windows could not wait for bridge loading. See launcher.log.", L"Windows 无法等待桥接加载。请查看 launcher.log。"},
    {L"Close Microsoft Flight Simulator before installing this update, then check for updates again.",
     L"安装此更新前请先关闭 Microsoft Flight Simulator，然后再次检查更新。"},
    {L"Could not check for updates. Please try again later.", L"无法检查更新。请稍后重试。"},
    {L"The installer could not be verified or started. Please check for updates again.", L"安装程序无法验证或启动。请再次检查更新。"},
    // Updates, donation and bug reports.
    {L"Check for updates", L"检查更新"},
    {L"Checking for updates...", L"正在检查更新…"},
    {L"Checking for updates in the background...", L"正在后台检查更新…"},
    {L"Taxi Cam is up to date.", L"Taxi Cam 已是最新版本。"},
    {L"Taxi Cam updates", L"Taxi Cam 更新"},
    {L"Taxi Cam update ready", L"Taxi Cam 更新已就绪"},
    {L"Taxi Cam update downloaded", L"Taxi Cam 更新已下载"},
    {L" has been downloaded and verified.\n\nClose Taxi Cam and start the installer now?",
     L"已下载并通过验证。\n\n是否现在关闭 Taxi Cam 并启动安装程序？"},
    {L"Update downloaded. Close Microsoft Flight Simulator, then choose Check for updates to install.",
     L"更新已下载。请关闭 Microsoft Flight Simulator，然后选择「检查更新」进行安装。"},
    {L"Close Microsoft Flight Simulator, then use Check for updates in the tray menu to install.",
     L"请关闭 Microsoft Flight Simulator，然后在托盘菜单中选择「检查更新」进行安装。"},
    {L"Unsaved settings", L"未保存的设置"},
    {L"Save your unsaved settings before installing?\n\nYes: save and continue.\nNo: discard "
     L"changes.\nCancel: keep the app open.",
     L"安装之前是否保存未保存的设置？\n\n是：保存并继续。\n否：放弃更改。\n取消：保持程序开启。"},
    {L"Taxi Cam — Settings", L"Taxi Cam — 设置"},
    {L"Bug report opened. Review the details and attach logs before submitting on GitHub.",
     L"已打开问题报告。请在 GitHub 提交之前核对内容并附上日志。"},
    {L"Taxi Cam bug report", L"Taxi Cam 问题报告"},
    {L"Could not open your browser. Open github.com/rthoms334/taxi-cam/issues and choose Bug report. "
     L"Attach logs from Diagnostics > Open log folder and include your Taxi Cam version.",
     L"无法打开浏览器。请打开 github.com/rthoms334/taxi-cam/issues 并选择 Bug report。"
     L"请通过「诊断 > 打开日志文件夹」附上日志，并注明你的 Taxi Cam 版本。"},
    {L"Could not open your browser. Visit https://github.com/rthoms334/taxi-cam.",
     L"无法打开浏览器。请访问 https://github.com/rthoms334/taxi-cam。"},
    {L"Taxi Cam on GitHub", L"Taxi Cam 的 GitHub 页面"},
    {L"Could not open your browser. You can also find the PayPal donation link in the Taxi Cam README.",
     L"无法打开浏览器。你也可以在 Taxi Cam 的 README 中找到 PayPal 捐赠链接。"},
    {L"Donate to Taxi Cam", L"向 Taxi Cam 捐赠"},
    // Language preference.
    {L"Language", L"语言"},
    {L"Follow Windows", L"跟随 Windows"},
    {L"Could not save the language preference.", L"无法保存语言偏好。"},
    {L"Interface language updated.", L"界面语言已更新。"},
    {L"Finish the current values before changing the language.", L"请先完成当前数值的填写，再更改语言。"},
};

// English is the source language, so an unknown string is already correct.
inline const wchar_t* chinese_message(const wchar_t* english) {
  if (!english || !english[0])
    return english;
  for (const auto& message : kChineseCatalog)
    if (std::wcscmp(message.english, english) == 0)
      return message.chinese;
  return english;
}

inline const wchar_t* tr(const wchar_t* english) {
  if (!speaking_chinese || !english)
    return english;
  return chinese_message(english);
}

// Segoe UI Variable has no Han glyphs, so the Han-linked family carries both scripts.
inline const wchar_t* interface_typeface() {
  return speaking_chinese ? L"Microsoft YaHei UI" : L"Segoe UI Variable";
}

inline UINT interface_charset() {
  return speaking_chinese ? GB2312_CHARSET : DEFAULT_CHARSET;
}

inline const wchar_t* menu_label(UiLanguage language) {
  switch (language) {
    case UiLanguage::follow_windows:
      return tr(L"Follow Windows");
    case UiLanguage::english:
      return L"English";
    case UiLanguage::simplified_chinese:
      return L"简体中文";
  }
  return L"English";
}
}  // namespace taxi_camera::standalone
