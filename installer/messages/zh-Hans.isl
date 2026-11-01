; Taxi Cam 安装程序文字。英文为源语言；taxi-cam.iss 的 [Languages] 中的每种语言
; 都有一个键相同的文件。%1 由安装程序填入。

[CustomMessages]
StartupOptionInvalid=/STARTUP 选项只能是 automatic 或 manual。
InstallFailed=安装失败。请关闭 MSFS 与 Taxi Cam 伴随程序，并检查所选路径。
SimulatorPageDescription=选择模拟器的 Content 目录
SimulatorPageSubCaption=请选择包含 FlightSimulator2024.exe 的目录。
SimulatorFolderLabel=模拟器 Content 目录：
StartupPageCaption=启动方式
StartupPageDescription=选择 Taxi Cam 的启动方式
StartupPageSubCaption=自动启动可以在重新运行安装程序时重试。若安装程序无法安全完成配置，Taxi Cam 仍会正常安装，你可以手动启动。
StartupAutomatic=配置随 MSFS 自动启动
StartupManual=手动启动；不更改现有启动项
XmlPageCaption=自动启动
XmlPageDescription=选择模拟器的 exe.xml
XmlPageSubCaption=安装程序会保留其他启动项并添加 Taxi Cam。也可以在所选路径新建 exe.xml。
XmlFileLabel=模拟器启动配置文件：
XmlFileFilter=XML 文件|*.xml|所有文件|*.*
SettingsPageCaption=已保存的设置
SettingsPageDescription=保留你的设置，或恢复默认设置
SettingsPageSubCaption=保留摄像头机型配置、校准、参考标线与键盘快捷键。摄像头帧率设为 10（最低 5，范围 5–60）。取消勾选可把已保存的设置恢复为默认值。
KeepExistingSettings=保留现有设置（推荐）(&K)
PreviousSettingsUnreadable=无法读取上一次的安装设置。请检查 installation.json。
SimulatorFolderInvalid=请选择包含 FlightSimulator2024.exe 的 Content 目录。
XmlPathInvalid=请选择以 exe.xml 结尾的完整路径。
ConfigureFailed=Taxi Cam 已安装，但安装程序未能完成配置：%1 请重新运行安装程序，或在使用 MSFS 时从开始菜单打开 Taxi Cam。
ManualStartupNotice=请先从开始菜单启动 Taxi Cam，再使用 MSFS。现有模拟器的启动项未作更改。你可以重新运行安装程序来配置自动启动。
InstalledSuccessfully=Taxi Cam 已成功安装。
KeepSettingsQuestion=保留 Taxi Cam 的设置？
KeepSettingsExplanation=为以后的安装保留摄像头机型配置、校准、参考标线与键盘快捷键。两种方式都会保留日志。
UninstallKeepSettings=保留设置（推荐）(&K)
UninstallKeepSettingsNote=卸载程序，但保留已保存的设置。
UninstallRemoveSettings=删除已保存的设置(&R)
UninstallRemoveSettingsNote=卸载程序，并删除已保存的设置。
UninstallCancel=取消(&C)
