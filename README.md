# NetCollcet-C

Windows 网络设备信息采集工具，使用 C11 和 Win32 原生界面编写。当前版本 **V0.1**，支持 Windows 10/11 x64。

从 Excel 读取设备清单，按 OS 工作表选择命令；多台设备并发采集，同一台设备按表格顺序执行命令。支持 SSH 和 Telnet，提供图形界面及命令行程序。

## 快速开始

1. 从仓库 Releases 下载 `NetCollcet-C-V0.1-Windows-x64.zip` 并解压。
2. 编辑 `C版采集模板.xlsx`，将示例地址和账号替换为实际设备信息。
3. 双击 `netcollect.exe`，选择模板、输出目录、并发设备数，检查模板后开始采集。

运行已编译程序不需要 Python、Excel 或第三方 DLL。模板可使用兼容 XLSX 格式的表格软件编辑。Telnet 明文传输账号及数据，仅应在可信管理网络使用。

## Excel 格式

第一个工作表为设备清单，其余工作表名称与 `OS类型` 一致。

| 列 | 要求与默认值 |
| --- | --- |
| ip地址 | 必填；支持 IPv4、IPv6 和主机名 |
| 用户名 | SSH 必填；仅密码认证的 Telnet 设备可留空 |
| 密码 | 登录密码；建议单元格设置为文本 |
| OS类型 | 必填；对应命令工作表名称 |
| 端口 | 可选；SSH 默认 22，Telnet 默认 23 |
| 编码 | 可选；默认 auto，支持 utf-8、gb18030、gbk、big5、latin1 |
| 超时秒 | 可选；默认 90，范围 1–3600 |
| 登录方式 | 可选；SSH 或 TELNET，不区分大小写，默认 SSH |

命令工作表使用 `命令`、`command`、`cmd` 表头，或直接在第一列逐行填写命令。空行和 `#` 开头的备注忽略。不执行工作簿宏；不支持旧版 XLS、加密工作簿或多行命令。

## 输出与日志

```text
输出目录/
  SW-Core_192.0.2.10/
    display version.txt
    display interface _ include up.txt
    _session.log
  Access_192.0.2.20_23/
    display version.txt
    _session.log
  collection_时间戳.log
  _summary.csv
  _known_hosts
```

设备名来自登录后返回的提示符。Telnet 目录始终带端口，SSH 非默认端口也附加端口。每条命令保存为独立文本：管道符 `|` 改为 `_`，空格保留，非法字符替换，重名自动编号。

输出统一为带 BOM 的 UTF-8，清理常见终端控制序列。有损转码会记录警告并保留 `.txt.raw` 原始数据。无法自动判断的编码应在模板中显式指定。

任务日志直接位于所选输出目录，记录连接、认证、命令、耗时及失败原因；运行日志不主动记录密码。启动日志位于程序目录 `logs`，不可写时使用 `%LOCALAPPDATA%\NetCollect\logs`。采集文件可能含设备敏感配置，请妥善保存。

## 命令行

```powershell
.\netcollect-cli.exe --version
.\netcollect-cli.exe --check .\C版采集模板.xlsx
.\netcollect-cli.exe --input .\C版采集模板.xlsx --output D:\采集结果 --workers 8
```

退出码：0 全部成功，1 存在设备失败，2 模板或文件错误，3 用户停止。

## 构建与测试

在 PowerShell 中执行，准备 Python 3.13 和联网环境。首次准备脚本会下载锁定版本的构建工具与依赖；依赖压缩包使用 SHA-256 校验。

```powershell
.\setup.ps1
.\build.ps1
python -m pip install -r requirements-dev.txt
.\test.ps1 -Gui
.\package.ps1
python -B ctests/smoke_release.py
```

发行文件为 `dist/NetCollcet-C-V0.1-Windows-x64.zip` 和同名 `.sha256` 校验文件。源码依赖下载到 `third_party/`，工具下载到 `.tools/`，均不进入 Git。

| 路径 | 用途 |
| --- | --- |
| `csrc/` | C 核心、Excel 解析、SSH/Telnet、日志、GUI/CLI |
| `ctests/` | C 单元测试、本地模拟设备、GUI 测试和性能测试 |
| `scripts/dependencies.json` | 上游依赖版本、下载地址与 SHA-256 |
| `setup.ps1` / `bootstrap.ps1` | 准备工具链 / 准备 C 依赖 |
| `build.ps1` / `test.ps1` / `package.ps1` | 编译 / 测试 / 打包 |
| `VERSION` | 程序、日志、发行包统一版本号 |

详细资料：[使用手册](README_C.md)、[构建说明](docs/BUILDING.md)、[测试记录](TEST_REPORT_C.md)、[更新记录](CHANGELOG.md)、[第三方许可](THIRD_PARTY_NOTICES.md)。

## 适用范围

已在本机模拟 SSH/Telnet 服务和实际 Win32 窗口完成自动化测试，未代表所有厂商设备或 Windows 版本均已验证。SSH 使用 WinCNG，不支持 Ed25519-only、SSH-1、私钥登录、MFA、强制修改密码或交互式 enable。首次 SSH 主机密钥采用首次信任，后续变化会拒绝连接。

仓库仅提供虚构示例设备；不要提交生产清单、账号密码、采集输出或运行日志。本项目主代码尚未声明开源许可证，第三方组件遵循其各自许可。
