# 第三方组件与许可

下列 C 组件静态链接进 Windows 可执行程序，构建时通过 `bootstrap.ps1` 下载。精确下载地址与 SHA-256 见 `scripts/dependencies.json`。发行包的 `licenses/` 目录包含上游原始许可全文。

| 组件 | 固定版本 | 许可 | 上游 |
| --- | --- | --- | --- |
| libssh2 | 1.11.1 | BSD 风格，见 COPYING | https://libssh2.org/ |
| Expat | 2.7.1 | MIT，见 expat/COPYING | https://github.com/libexpat/libexpat |
| miniz | 3.0.2 | MIT，见 LICENSE | https://github.com/richgel999/miniz |
| libtelnet | 0.23 | 上游声明公共领域，见 COPYING | https://github.com/seanmiddleditch/libtelnet |

构建使用 Zig/Clang、CMake 和 Ninja；开发测试使用 Python、openpyxl、Paramiko 和 Pillow。这些工具与测试依赖不随发行包分发，也不是最终程序的运行依赖。工具及其依赖遵循各自上游许可。

本文件是组件索引，不替代上游许可正文。本项目主代码未声明开源许可证。
