# NetCollcet-C V0.1

Windows x64 首次发行，提供原生 C 图形界面和命令行网络设备采集程序。

## 主要内容

- Excel 驱动的 SSH/Telnet 混合采集，按 OS 分组命令。
- 多设备并发，同一设备命令顺序执行。
- 设备名与管理地址目录，命令独立 UTF-8 文本。
- 中文编码处理、分页、超时取消与不完整结果保留。
- 任务日志直接保存在所选输出目录，便于排错。
- 附虚构示例模板、使用说明、测试报告和第三方许可。

## 使用

下载 `NetCollcet-C-V0.1-Windows-x64.zip`，对照同名 `.sha256` 文件校验，解压后运行 `netcollect.exe`。无需安装 Python、Excel 或第三方 DLL。

## 验证与限制

本机 Windows x64 构建，执行 C 核心/日志测试、本地 SSH/Telnet 集成测试、实际 Win32 GUI 测试和发行冒烟测试，详见仓库 `TEST_REPORT_C.md`。未进行生产硬件及其他 Windows 主机联调。SSH WinCNG 后端不支持 Ed25519-only、私钥、MFA 或交互式 enable；Telnet 为明文协议，应仅用于可信管理网络。
