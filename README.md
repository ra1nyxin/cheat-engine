<p align="center">
  <a href="https://github.com/ra1nyxin/cheat-engine">
    <img src="Cheat%20Engine/images/celogo.png" alt="Cheat Engine 徽标" />
  </a>
</p>

<h1 align="center">Cheat Engine</h1>

Cheat Engine 是一个面向游戏和应用程序分析、调试与个人研究的开发环境。本仓库仅供个人研究和使用，不面向上游项目提交更改。

## 下载

- **[最新自动构建](https://github.com/ra1nyxin/cheat-engine/releases/tag/ci-latest)**
- [全部发行版](https://github.com/ra1nyxin/cheat-engine/releases)

每次推送到 `main` 或 `master` 时，GitHub Actions 会构建并分别发布以下资产，每个 ZIP 都有对应的 SHA-256 校验文件：

- `CheatEngine-Windows-x86-*.zip`：32 位 Windows 主程序。
- `CheatEngine-Windows-x64-*.zip`：通用 64 位 Windows 主程序，推荐大多数电脑使用。
- `CheatEngine-Windows-x64-AVX2-*.zip`：支持 AVX2 指令集的 64 位主程序。
- `CheatEngine-DBK-Unsigned-x86-x64-*.zip`：单独提供的未签名 DBK32/DBK64 内核驱动。

三个主程序包各自只包含一个 Cheat Engine 主程序，不需要同时下载。为保证 64 位程序分析 32 位目标等跨位数功能正常，各包仍会保留必要的 32/64 位辅助组件。

## DBK 驱动

Release 中的 DBK 驱动由 GitHub Actions 从当前 `DBKKernel` 源码构建，但不会使用仓库作者的证书或任何私钥进行签名，也不会默认放入三个主程序包。需要使用时，请下载单独的未签名驱动包，将与系统架构对应的 `DBK32.sys` 或 `DBK64.sys` 放入 Cheat Engine 的 `bin` 目录。

驱动使用前需要自行签名并安装自己的测试证书。现代 64 位 Windows 通常还需要以管理员身份执行以下命令开启测试签名模式，然后重启：

```bat
bcdedit /set testsigning on
```

启用 Secure Boot 的电脑通常需要先关闭 Secure Boot。自签名驱动不等同于微软签名驱动，仅建议在个人研究或隔离测试环境中使用；开启内存完整性/HVCI 时，旧式内核驱动仍可能因兼容性检查而无法加载。

## 从源码构建

1. 从 [Lazarus 2.2.2 下载页](https://sourceforge.net/projects/lazarus/files/Lazarus%20Windows%2064%20bits/Lazarus%202.2.2/) 下载并安装 `lazarus-2.2.2-fpc-3.2.2-win64.exe`。
2. 安装 `lazarus-2.2.2-fpc-3.2.2-cross-i386-win32-win64.exe`，以启用 32 位 Windows 交叉编译。
3. 在 Lazarus 中打开 `Cheat Engine/cheatengine.lpi`。
4. 使用“运行 -> 构建”或 `Shift+F9` 构建；需要多个架构时，在“运行 -> 编译多个模式”中选择发布模式。
5. 若要在 Windows 上从 IDE 运行或调试程序，请以管理员身份启动 Lazarus。

## 辅助组件

主程序可独立构建。以下组件按需单独构建，以启用对应功能：

- `speedhack.lpr`：速度修改功能，需要构建 32 位和 64 位 DLL。
- `luaclient.lpr`：Lua 客户端功能，需要构建 32 位和 64 位 DLL。
- `DirectXMess.sln`：Direct3D 叠加层和截图功能，需要构建 32 位和 64 位版本。
- `DotNetCompiler.sln`：Lua 的 `cscompile` 命令。
- `MonoDataCollector.sln`：.NET/Mono 环境检查功能，需要构建 32 位和 64 位 DLL。
- `DotNetDataCollector.sln`：.NET 符号功能，需要构建 32 位和 64 位可执行文件。
- `DotNetInvasiveDataCollector.sln`：运行时 JIT 支持。
- `Java/CEJVMTI.sln`：Java 进程检查功能，需要构建 32 位和 64 位 DLL。
- `tcclib.sln`：脚本中的 `C` 与 `CCODE` 支持，需要构建 32-32、64-32 和 64-64 组合。
- `VEHDebug/vehdebug.lpr`：VEH 调试接口，需要构建 32 位和 64 位 DLL。
- `DBKKernel/DBKKernel.sln`：内核模式功能。自动构建使用 `Release without sig` 配置生成 DBK32/DBK64 驱动，签名和测试模式要求见上文。

`.sln` 工程需要 Visual Studio。内核驱动、进程调试和内存修改功能可能需要管理员权限，并受 Windows 安全策略影响。

## 相关链接

- [Cheat Engine 官方网站](https://www.cheatengine.org)
- [官方论坛](https://forum.cheatengine.org)
- [项目维基](https://wiki.cheatengine.org/index.php?title=Main_Page)
