# 构建、测试和发布

## 环境

- Windows 10/11 x64，PowerShell 7 推荐。
- Python 3.13，可通过 `python` 调用，并具有 pip。
- 首次准备需要访问 PyPI、GitHub 和 libssh2 官方下载站。
- GUI 测试需要可创建原生窗口的 Windows 会话。

程序本体由 C11 源码构建，Python 只用于工具准备和测试，不是发行程序运行依赖。

## 一次性准备

```powershell
.\setup.ps1
python -m pip install -r requirements-dev.txt
```

`requirements-build.txt` 固定 Zig、CMake、Ninja 版本。`setup.ps1` 将工具安装在项目 `.tools/python`，随后调用 `bootstrap.ps1`。后者按 `scripts/dependencies.json` 下载第三方 C 源码并校验压缩包 SHA-256。已有源码目录视为本地工作副本，不会自动覆盖；需要重新验证下载包时，在新的干净检出目录运行准备脚本。

## 日常命令

```powershell
.\build.ps1
.\test.ps1 -Gui
.\test.ps1 -Benchmark
.\package.ps1
python -B ctests/smoke_release.py
```

`build.ps1` 配置 Release 构建并运行 C 核心和日志测试。`test.ps1` 运行 C 测试、本地 SSH/Telnet 集成测试及 PE 导入表检查；`-Gui` 加入实际原生窗口与取消操作测试，`-Benchmark` 加入混合协议性能对比。测试使用本机回环模拟器，不会使用工作区中的生产设备清单。

`package.ps1` 重新生成虚构示例模板，收集可执行文件、文档及上游许可，按显式白名单打包，输出版本 ZIP 和 SHA-256。`smoke_release.py` 检查打包目录中的程序版本、示例模板及原生窗口启动退出。

输出位置：

- `build/`：CMake/Ninja 编译结果。
- `test-output/`：模拟设备输出、指标及窗口截图，不提交 Git。
- `dist/NetCollect-C/`：解压后的发行目录，内部沿用兼容路径。
- `dist/NetCollcet-C-V0.1-Windows-x64.zip`：面向用户的版本发行包。

## 版本与发布

`VERSION` 是单一版本来源，格式为 `V主版本.次版本`。CMake 根据该文件生成 `version.h`，窗口、命令行和日志使用相同版本。修改版本后必须重新运行构建和打包。

发布前更新 `CHANGELOG.md` 和 `TEST_REPORT_C.md`，执行完整测试与发行冒烟测试，再创建与 `VERSION` 完全一致的 Git 标签。上传 ZIP 和对应 `.sha256` 到 GitHub Release。校验下载包：

```powershell
Get-FileHash .\NetCollcet-C-V0.1-Windows-x64.zip -Algorithm SHA256
```

`.github/workflows/windows.yml` 在 Windows runner 上准备工具、编译、测试、打包并保留发行工件。CI 配置本身不意味着远端测试已经通过，应以实际 Actions 执行结果为准。

## 安全边界

`.gitignore` 排除任意 Excel 文件，仅允许根目录虚构示例 `C版采集模板.xlsx`；同时排除输出、日志、私钥、本机工具及依赖源码。提交前仍需审查暂存区，避免在源码或说明中写入生产账号和设备配置。不要用 `git add -f` 强行添加真实清单。
