# CrossWin：编译、运行与验证教程

本仓库已完成 Stage 1～7D，以及 Stage 8/9 的 proxy 窗口内键盘、滚轮和低延迟
presentation 合并：Linux Weston 保有窗口、位置、激活和拖动的 canonical
state；Windows Agent 为每个 Linux toplevel/popup 创建独立原生 HWND，通过 TCP
接收 framebuffer、damage 与 presentation，并把鼠标事件回传 Linux。

当前支持 wl_shm、subsurface、alpha、popup、多窗口生命周期、Linux-owned drag、
CWNP v6 以及有理数逻辑→物理 DPI presentation。GPU/dmabuf、共享内存高性能传输
和最终压力恢复仍属后续阶段。

## 目录说明

```text
geometry/                  Stage 1：纯 C11 Geometry Oracle
crosswin/
├── common/                CWNP v6 显式 little-endian 协议与 stream decoder
├── fake-server/           Linux TCP fake server、测试图案、presentation history
├── windows-agent/         C++17 Win32 + Winsock + GDI proxy HWND
├── tests/                 协议、presentation、pointer round-trip 测试
└── Makefile
```

## 一、Linux 端准备与编译

### 1. 依赖

Linux 端只需要一个支持 C11 的 C 编译器和 GNU Make。Debian/Ubuntu 示例：

```bash
sudo apt update
sudo apt install build-essential
```

确认编译器可用：

```bash
cc --version
make --version
```

### 2. 编译 Geometry Oracle

在仓库根目录执行：

```bash
make -C geometry
make -C geometry test
```

预期输出：

```text
geometry tests: PASS
cases: ...
```

### 3. 编译 CrossWin Linux 组件

```bash
make -C crosswin
```

生成的程序位于 `crosswin/build/`：

```text
fake-server       Linux TCP 服务端
protocol_test     协议 decoder 测试
session_test      presentation history、坐标与 fake grab 测试
scripted-agent    Stage 2 自动 TCP client
input-agent       Stage 3 自动 TCP client
```

### 4. 运行 Linux 自动测试

```bash
make -C crosswin test
make -C crosswin integration
make -C crosswin input-integration
make -C crosswin stage7d-integration
make -C crosswin stage8-integration
make -C crosswin sanitize
```

各命令的意义：

- `test`：检查 TCP decoder 拆包/粘包、非法 payload、signed pointer
  coordinate、presentation history、stale sequence 和 Geometry Oracle。
- `integration`：用真实 TCP 回环执行 Stage 2 脚本；每条 `WINDOW_PRESENT`
  必须等到对应 ACK 后才发送下一条。
- `input-integration`：验证 Stage 3 端到端 click crosshair 与 Linux-owned
  fake drag。
- `sanitize`：以 AddressSanitizer 和 UndefinedBehaviorSanitizer 执行 C11
  测试。

成功时应看到：

```text
protocol tests: PASS
session tests: PASS
scripted presentation integration: PASS
pointer round-trip integration: PASS
geometry tests: PASS
```

## 二、Windows 端准备与编译

### 1. 安装组件

在 Windows 虚机安装 Visual Studio 2022 Community，或仅安装 Build Tools。
安装器中至少勾选：

- **Desktop development with C++**
- **MSVC x64 build tools**
- **Windows 10 SDK** 或 **Windows 11 SDK**

不需要 Qt、SDL、GLFW、Boost、CMake、MinGW、DirectX SDK 或第三方图形库。

### 2. 进入 MSVC x64 命令行

从开始菜单打开：

```text
x64 Native Tools Command Prompt for VS 2022
```

进入 Windows 上的仓库副本：

```bat
cd C:\path\to\LWforWindowsOS\crosswin
mkdir build
```

> `crosswin-agent.exe` 只能运行在 Windows；Linux 端不需要、也不能构建
> Win32 `HWND` 程序。

### 3. 编译 Windows Agent

最简单的方式是在 Windows 文件资源管理器中双击：

```text
crosswin\build-windows-agent.cmd
```

脚本会自动查找 Visual Studio 的 x64 C++ 工具链，创建 `build\`，并生成：

```text
crosswin\build\crosswin-agent.exe
```

也可以从“x64 Native Tools Command Prompt for VS 2022”手动执行以下命令。

先以 C11 编译共享协议和 Geometry Oracle：

```bat
cl /nologo /std:c11 /W4 /WX /c common\protocol.c /Fobuild\protocol.obj
cl /nologo /std:c11 /W4 /WX /c ..\geometry\geometry.c /Fobuild\geometry.obj
```

再以 C++17 编译链接 Agent：

```bat
cl /nologo /std:c++17 /W4 /WX /EHsc /Icommon /Iwindows-agent /I..\geometry ^
  windows-agent\main.cpp ^
  windows-agent\protocol.cpp ^
  windows-agent\proxy_window.cpp ^
  build\protocol.obj ^
  build\geometry.obj ^
  /link ws2_32.lib user32.lib gdi32.lib ^
  /out:build\crosswin-agent.exe
```

成功后产物为：

```text
crosswin\build\crosswin-agent.exe
```

链接库用途：

- `ws2_32.lib`：Winsock TCP / `WSAAsyncSelect`
- `user32.lib`：Win32 window、鼠标、capture、消息循环
- `gdi32.lib`：top-down BGRA `StretchDIBits` 绘制

## 三、Stage 2：Linux → Windows 画面验证

### 1. Linux 启动 fake server

Linux 主机上：

```bash
cd /path/to/LWforWindowsOS/crosswin
make
./build/fake-server \
  --listen 0.0.0.0 \
  --port 44600 \
  --script-stage2 \
  --trace-protocol \
  --trace-present
```

若 Linux 防火墙启用，允许 Windows VM 连入 TCP 44600。例如使用 UFW：

```bash
sudo ufw allow 44600/tcp
```

### 2. Windows 连接 Agent

在 Windows 虚机的 x64 Native Tools 命令行中：

```bat
build\crosswin-agent.exe --host <Linux主机IP> --port 44600 --trace-protocol --trace-present
```

例如：

```bat
build\crosswin-agent.exe --host 192.168.122.1 --port 44600 --trace-protocol --trace-present
```

Windows Agent 是客户端，Linux fake-server 是监听端。当前只接受 IPv4 地址。

### 3. 观察结果

服务端会按固定脚本发送：

```text
seq=1  src=[0,0   800x600]  dst=[300,200 800x600]
seq=2  src=[100,0 700x600]  dst=[0,200   700x600]
seq=3  src=[220,0 580x600]  dst=[0,300   580x600]
seq=4  src=[400,100 400x400] dst=[200,100 400x400]
seq=5  visible=false
seq=6  恢复完整 800x600
DESTROY
```

重点检查第 3 条 presentation：

- HWND 位置、大小必须为 `[0,300 580x600]`。
- 第一列像素必须来自 Linux surface 的 `x=220`，不能来自 `x=0`。
- 图案不能上下翻转，红蓝通道不能交换。
- 每条 presentation 都应在 Windows apply 后返回 `WINDOW_PRESENT_ACK`。
- 第 5 条必须隐藏窗口，第 6 条必须恢复显示，最终 `DESTROY` 后窗口关闭。

Windows remote display 请设置为 **100% 缩放**。当前阶段的 presentation
contract 是 scale=1，尚未实现 Windows fractional-DPI presentation。

## 四、Stage 3：Windows → Linux Pointer 验证

Stage 3 使用交互模式，服务端不会执行 Stage 2 自动 destroy：

```bash
cd /path/to/LWforWindowsOS/crosswin
./build/fake-server \
  --listen 0.0.0.0 \
  --port 44600 \
  --interactive \
  --trace-present \
  --trace-input
```

Windows：

```bat
build\crosswin-agent.exe --host <Linux主机IP> --port 44600 --trace-input --trace-present
```

初始 presentation 是：

```text
source      = [220,0 580x600]
destination = [0,300 580x600]
sequence    = 42
```

在 proxy HWND client area 点击：

```text
client = (133,211)
```

Linux 日志应显示：

```text
[input map] surface=(353,211) global=(2053,511)
```

随后 Linux 会在原始 framebuffer 的 `(353,211)` 画一个 21×21 黄色 crosshair
并重新发送 `WINDOW_FRAME`。Windows 上的 crosshair 应在点击处出现。

按住左键继续拖动时，Windows 只发送 pointer event；Linux 用 Geometry Oracle
更新 canonical global window geometry，再发回新的 `WINDOW_PRESENT`。Windows
不应在收到 `WM_MOUSEMOVE` 时擅自改变窗口位置。

## 五、常用问题排查

### Windows Agent 无法连接

依次检查：

1. Linux server 是否先启动并监听 `0.0.0.0:44600`。
2. `--host` 是否是 Linux 主机在 VM 可路由网络中的 IPv4 地址。
3. Linux 防火墙是否允许 `44600/tcp`。
4. KVM 网络是否允许虚机主动连接 host。

### 窗口位置或大小不正确

开启双方 `--trace-present`，核对：

```text
[present tx]    Linux 发出的 src/dst/seq
[present apply] Windows 应用的 src/dst/seq
```

`src/dst` 始终是逻辑尺寸；若启用了 Stage 7D 的 output scale，Windows 日志还会
显示相应的 `physical-hwnd`。不要把 logical `--crosswin-x` 改成 Windows 物理像素。

### 点击后 crosshair 有偏移

开启 Linux `--trace-input`。检查同一个 `presentation sequence` 下：

```text
client + source origin = surface
```

例如 `client=(133,211)` 和 `src=(220,0)` 必须得到 `surface=(353,211)`。若
`presentation sequence` 已超出 Linux 保存的最近 64 条 history，服务端会明确
打印 stale-presentation drop，而不会偷偷使用最新 geometry。

## 六、当前限制

- 目前只有一个 remote Windows output；它可以放在 Linux logical desktop 的 right、
  left、above 或 below。
- Wayland export 仍限支持的 wl_shm 场景；GPU/dmabuf 在 Stage 7E 实现。
- 已支持**获焦 Crosswin proxy HWND** 的窗口内键盘和滚轮；IME、剪贴板、视频编码
  和跨系统鼠标 ownership 仍未实现。Deskflow 仍负责物理键盘/鼠标跨机。
- 这不是 production transport：没有 TLS、认证、重连协议或 damage/frame compression。

详细协议、测试结构和 Windows 视觉检查说明也见 [crosswin/README.md](crosswin/README.md)。
Stage 7D 的完整中文命令和 Windows Gate 见 [docs/stage7d-dpi-test.txt](docs/stage7d-dpi-test.txt)。
Stage 8/9 的键盘、滚轮和连续拖动 Gate 见 [docs/stage8-input-stage9-present-test.txt](docs/stage8-input-stage9-present-test.txt)。
