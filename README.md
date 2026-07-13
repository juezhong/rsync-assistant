# rsync-assistant

`rsync-assistant` 是一个本地优先的 C++ 终端传输助手。它把任务、日志和传输进程放到本机守护进程中管理，再通过全屏 TUI 创建、预检、确认和执行 `rsync` 传输。

它不会让 AI 执行 shell 命令。所有传输命令都由受限选项生成，并在执行前显示给你确认。

## 适用范围

- macOS 与 Linux；Windows/MSYS2 暂未支持。
- 本地复制、`rsync over SSH`，以及经用户明确授权的 rsync daemon。
- 远端部署 `rsync-assistant` 后，可通过 SSH 做目录浏览、任务状态查询和受控测速。

## 快速开始

需要：CMake 3.20+、C++20 编译器、`make`、SQLite **开发文件**、OpenSSH（`ssh`/`scp`）和 `rsync`。首次配置还会从 GitHub 下载 FTXUI。

克隆后先取得固定版本的 rsync 子模块：

```sh
git clone --recurse-submodules git@github.com:juezhong/rsync-assistant.git
cd rsync-assistant
```

如果已经克隆过仓库：

```sh
git submodule update --init --recursive
```

常见 Linux 依赖安装命令：

```sh
# Debian / Ubuntu: bundled rsync 的完整基础构建链
sudo apt install build-essential cmake git make libsqlite3-dev rsync openssh-client \
  autoconf automake gawk libxxhash-dev liblz4-dev libzstd-dev libssl-dev

# Fedora / RHEL
sudo dnf install gcc-c++ cmake git make sqlite-devel rsync openssh-clients \
  autoconf automake gawk xxhash-devel lz4-devel libzstd-devel openssl-devel

# Arch Linux
sudo pacman -S --needed base-devel cmake git sqlite rsync openssh xxhash lz4 zstd openssl
```

macOS 当前默认使用系统 `rsync` 进行构建测试：

```sh
./scripts/build-macos.sh
```

Linux 默认同时构建 `third_party/rsync` 中固定版本的私有 rsync：

```sh
./scripts/build-linux.sh
```

也可以使用标准 CMake：

```sh
cmake -S . -B build
cmake --build build --parallel
```

### 构建脚本做了什么？

`scripts/build-linux.sh` 和 `scripts/build-macos.sh` 不是安装脚本，也不会修改系统配置；它们只是把下面两条 CMake 命令和合适的默认值固定下来，并打印实际使用的目录与选择：

| 动作 | Linux 脚本 | macOS 脚本 |
| --- | --- | --- |
| 定位源码目录 | 脚本上一级，即仓库根目录 | 相同 |
| 构建目录 | 默认 `./build`，可用 `BUILD_DIR=/path` 覆盖 | 相同 |
| 配置 CMake | `cmake -S <root> -B <build>` | 相同 |
| rsync 选择 | 默认 `RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC=ON`，构建 `third_party/rsync` | 默认 `OFF`，使用系统 rsync 进行首次构建 |
| 编译 | 先并行完成 bundled rsync，再并行编译主程序与 FTXUI | `cmake --build <build> --parallel <min(逻辑 CPU, 16)>` |

Linux 的 bundled 选项开启时，CMake 会在 `build/private-rsync/` 调用子模块自带的 `configure` 和 `make`，生成私有 `rsync`；程序运行时优先使用它。脚本开头的 `set -eu` 表示任一步失败都会立即停止，也会把未定义变量当成错误，避免在失败后继续产生不完整构建。

脚本内部自动把并发编译数设为 `min(逻辑 CPU 数, 16)`，并在开始时打印最终数字；用户不需要设置线程参数。Linux bundled 模式会先用同一个 job 数并行完成 rsync，再并行编译主程序与 FTXUI，避免 rsync 的 `configure` 输出和 FTXUI 编译日志混在一起。主程序阶段的多行输出仍可能交错，这是多个并行编译进程同时输出日志的正常现象，不代表脚本启动了后台传输或其他异步任务。

#### Linux bundled-rsync prerequisites

运行 `scripts/build-linux.sh` 时，脚本会先检查并明确报告缺失项：

- 始终需要：`cmake`、`make`、`cc`、`c++`、`git`、`ssh`、`scp`，以及可编译和链接的 SQLite 开发文件。
- 默认 bundled rsync 额外需要：`aclocal`（来自 `automake`）、`autoconf`、`autoheader` 和 `gawk`。
- bundled rsync 的上游 Git 构建还默认要求完整的 `xxhash`、`lz4`、`zstd` 与 OpenSSL 开发库；脚本会逐项检查头文件和链接库，并打印 Debian/Ubuntu、Fedora/RHEL、Arch 的准确包名。`acl` 与 `attr` 仍是可选元数据能力。

rsync 上游 Git 源码本来会用 Python Markdown 模块生成 manpage，但 rsync-assistant 的私有 rsync 构建明确使用 `--disable-md2man` 跳过它：manpage 与传输二进制无关，因此不需要安装 Python 或 Python Markdown 包。

因此如果看到 `aclocal: command not found`，应安装 `automake`；不要只重跑 CMake。脚本会在 CMake 开始前就报出这一点。

常用的显式调用如下：

```sh
# 使用另一处构建目录，不污染默认 ./build
BUILD_DIR="$HOME/build/rsync-assistant" ./scripts/build-linux.sh

# Linux 上临时明确改为系统 rsync；不是默认行为
RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC=OFF ./scripts/build-linux.sh
```

## 构建排错

### `Could NOT find SQLite3 (missing: SQLite3_INCLUDE_DIR SQLite3_LIBRARY)`

系统只安装了 SQLite 运行库，缺少头文件和链接库。安装发行版对应的 SQLite 开发包后，删除旧构建目录再重试：Debian/Ubuntu 使用 `libsqlite3-dev`，Fedora/RHEL 使用 `sqlite-devel`，Arch 使用 `sqlite`。

```sh
rm -rf build
./scripts/build-linux.sh
```

### bundled rsync 缺少 `xxhash.h`、`lz4.h`、`zstd.h` 或 OpenSSL

这是 rsync 上游 Git 构建默认启用的校验/压缩/加密加速能力。新版 Linux 脚本会在 CMake 前直接显示缺失库和对应包名；安装后直接重跑脚本即可。以 Debian/Ubuntu 为例：

```sh
sudo apt install libxxhash-dev liblz4-dev libzstd-dev libssl-dev
./scripts/build-linux.sh
```

### `Found SQLite3 ... but target SQLite3::SQLite3 was not found`

这是部分 Linux 发行版携带的旧版 CMake `FindSQLite3` 模块只提供变量、不提供导入 target 的兼容性问题。请拉取包含兼容修复的最新 `main`，然后清理并重新配置：

```sh
git pull --ff-only
rm -rf build
./scripts/build-linux.sh
```

当前构建脚本同时兼容 `SQLite::SQLite3`、`SQLite3::SQLite3` 和只有变量的 FindSQLite3 模块。

### FTXUI 下载失败

首次 CMake 配置会通过 Git 下载 FTXUI。确认该机器可访问 GitHub，或在已联网的环境预先完成一次 CMake 配置后再使用同一个 `build/` 目录。

### 私有 rsync 构建失败

Linux 默认构建 `third_party/rsync`。先确认子模块已初始化、C/C++ 编译器和 `make` 已安装；如仅需临时使用系统 rsync，可明确回退：

```sh
cmake -S . -B build -DRSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC=OFF
cmake --build build --parallel
```

运行程序会在需要时启动同一状态目录下的后台守护进程，并直接进入 TUI：

```sh
./build/rsync-assistant
```

首次建议先创建一份可编辑配置：

```sh
./build/rsync-assistant --write-default-config "$HOME/.config/rsync-assistant/settings.toml"
./build/rsync-assistant --config "$HOME/.config/rsync-assistant/settings.toml"
```

默认状态目录是系统临时目录下的 `rsync-assistant`。要隔离一套任务历史、socket、日志和设置，可把 `--state-dir` 放在命令的最后：

```sh
./build/rsync-assistant --state-dir "$HOME/.local/state/rsync-assistant"
```

## 第一次创建任务

1. 在主界面按 `n`，进入三步新建任务面板。
2. 输入本地源路径和目标路径；普通远端格式是 `host:/absolute/path`，会使用你已有的 SSH 配置与 ssh-agent。
3. 可按 `F2` / `F3` 选择源和目标。源选择器支持 `h`、`j`、`k`、`l` 浏览，`Space` 多选，`/` 搜索，`g` 显示隐藏文件，`i` 包含自动忽略项，`F` 切换扁平化。
4. 在第二步选择压缩、dry-run、删除和项目排除项。`.git` 不会默认传输；检测到的 `build/`、虚拟环境、Rust `target/` 和常见依赖目录也默认排除。
5. 审阅生成的命令，按 Enter 创建 Ready Task。创建任务不会立即传输。
6. 主界面选中该任务后按 Enter：先执行 dry-run 并显示结果；再次按 Enter 才开始真正传输。

默认 `dry-run` 已开启。启用 `--delete` 时，真实执行前还必须输入 `DELETE`。

## 主界面快捷键

| 按键 | 操作 |
| --- | --- |
| `n` | 新建任务 |
| Enter | 对 Ready 任务预检；对已预检任务开始传输 |
| `p` | 暂停或继续 rsync 任务 |
| `x` | 停止当前任务 |
| `e` | 重新准备失败或中断的 rsync 任务（利用 rsync partial 续传） |
| `w` | 等待当前任务结束并刷新状态 |
| `c` | rsync 失败后，明确确认使用系统 `scp` 保底 |
| `s` | 打开设置；`Ctrl-S` 保存 |
| `d` | 生成网络 rsync daemon 部署模板，不会启动网络监听 |
| `R` | 查询已部署远端助手的任务状态 |
| `r` | 刷新任务与日志 |
| `q` | 退出 TUI |

`scp` 没有 rsync 的 dry-run、暂停或续传保证，因此只能在失败后明确确认使用。

## 远端与 rsync daemon

普通 SSH 远端不需要安装本软件：填入 `host:/path` 后，预检会通过 SSH 探测远端 `rsync`。

如两端都安装了本软件，远端额外提供目录浏览、状态查看和测速控制；数据传输仍由 rsync 完成。程序不会自动打开网络 rsync daemon。需要时只生成供你审核的模板：

```sh
./build/rsync-assistant --write-rsyncd-config ./rsyncd.conf
```

对于受信任局域网的 direct daemon 目标，格式为 `host::module/path`。必须在任务里勾选 **Trust direct rsync daemon on LAN**。若还填写同一主机的 **Paired SSH destination**，程序会在后台以助手专用临时数据测速：缓存中只有 daemon 比 SSH 快到设定阈值时才选 daemon；否则使用更简单的 rsync over SSH。默认测速为 64 MiB、15 秒超时、24 小时缓存，可在设置页或 TOML 修改。

## 守护进程与故障恢复

通常不必手动管理守护进程。下面两个模式用于排错或分开运行：

```sh
./build/rsync-assistant daemon --state-dir /path/to/state
./build/rsync-assistant tui --state-dir /path/to/state
```

一个状态目录只能有一个守护进程和一个前台 TUI。守护进程重启后，运行中的任务会显示为 Interrupted，可用 `e` 重新预检并续传。

完整执行日志保存在 SQLite 任务库中；本地目标目录可写时，也会写入 `.rsync-assistant-<task-id>.log`。

## 安全边界

- 不接受任意 shell 片段，传输参数来自固定的受限选项。
- API key 只从 TOML 读取；文件不是 owner-only `0600` 时，AI 解释功能会自动禁用。
- 本机受管 rsync daemon 仅绑定 loopback，并随守护进程结束。
- 网络可达 daemon 需要你自行审核绑定地址、认证、允许来源和防火墙规则。
- 除非明确确认，软件不会使用 `scp`，也不会开启远端 daemon。

## 测试

测试源保留为本地开发资料，不会提交到仓库。若工作树中有 `tests/`，可运行：

```sh
ctest --test-dir build --output-on-failure
```

## 许可证

见 [LICENSE](LICENSE)。`third_party/rsync` 保持其上游许可证和版权声明。
