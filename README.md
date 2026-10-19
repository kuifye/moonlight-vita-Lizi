# Vita Moonlight（中文增强 + 性能修复版）

这是 [xyzz/vita-moonlight](https://github.com/xyzz/vita-moonlight) 的一个 fork，在原有功能基础上，
针对 PlayStation Vita 的串流体验做了重要改进：**修复画面突变时的卡死问题**、**优化延迟与画质**、**内置简体中文界面**。

Moonlight-Vita by Lizi（梨子制作打包）。

---

## 本 fork 的核心改进

### 1. 修复「画面突变 → 单核满载 → 帧冻结卡死」的致命 bug

**特性**（`third_party/moonlight-common-c/src/RtpVideoQueue.c`）：
- 收包时对数据分片做快速判断，**缺包直接等待、数据全齐直接播放**，避免每收一个包都执行一次 Reed-Solomon FEC 矩阵重建。当游戏画面剧烈变化（进入加载界面、切换场景）产生超大关键帧时，不再触发单核计算风暴导致画面冻结。
- 默认关闭本地 FEC 重建（`fec_recovery_mode = 0`），弱网下以「掉帧 + 等待关键帧」保证流畅，而非计算卡死。
- 保留可选轻量纠错模式，限制分片规模、缺包数量和尝试次数。

### 2. 延迟与画质优化

- **收包 / 解码 / 上屏分离**（`enable_split_decode_thread`），避免绘制画面时阻塞收包。
- **帧节奏控制**（frame pacer）独立线程监控队列，积压时主动丢弃过期帧，防止延迟越积越高。
- **关键帧请求限频**（至少间隔 250ms，合并重复请求），防止恢复请求堆积。
- **连续丢帧上限改为细档**：5 / 8 / 10 / 13 / 15 / 18 / 20 / 30 / 40 / 60 / 80 / 100 / 120 / 200 / 300，可在菜单中精细调节。
- **网络预设档与 FEC 策略对齐**：预设档不再要求服务器多发校验包。

### 3. 简体中文界面

- 界面完整汉化（设置、连接、提示等）。
- 中文字体采用 **思源黑体（Noto Sans CJK SC）**，SIL Open Font License 1.1 许可，可自由再分发。

---

## 构建

需要 [VitaSDK](https://vitasdk.org/) 交叉编译工具链。

```bash
export VITASDK=/path/to/vitasdk
export PATH=$VITASDK/bin:$PATH

# 第三方库（moonlight-common-c / enet / inih / mdnsniff）已随源码提交，
# 无需 git submodule 初始化。直接 clone 本仓库即可获得完整可构建源码。

# 生成中文字体子集（需要 fontTools 与思源黑体源文件）
pip install fonttools
# 思源黑体（Noto Sans CJK SC Regular）可从此下载：
#   https://cdn.jsdelivr.net/gh/notofonts/noto-cjk@main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf
python3 gen_cjk_font.py . --source-font /path/to/NotoSansCJKsc-Regular.otf

# 构建
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

产物为 `build/moonlight.vpk`，用 VitaShell 安装即可。

---

## 许可证

本项目遵循 **GNU General Public License v3（GPL v3）**，完整条款见 [LICENSE](LICENSE)。

- 你可以自由使用、修改和**再分发**本软件（包括二进制），但必须：
  1. 保留本许可证及原作者版权声明；
  2. 公开你修改后的**完整源码**；
  3. 保留原始项目出处（见下方「致谢」）。

- 中文字体「思源黑体」遵循 **SIL Open Font License 1.1**，可自由使用与再分发。

---

## 致谢

- 上游项目：[xyzz/vita-moonlight](https://github.com/xyzz/vita-moonlight)（GPL v3）
- 依赖：[moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c)、[enet](https://github.com/cgutman/enet)、[inih](https://github.com/benhoyt/inih)、[mdnsniff](https://github.com/AorsiniYT/mdnsniff)
- 中文字体：[Noto Sans CJK SC / 思源黑体](https://github.com/notofonts/noto-cjk)（SIL OFL 1.1）

---

## AI 使用声明

本项目在开发过程中使用了以下 AI 辅助工具进行代码分析与修改：

- **GPT-6-Astra**
- **DeepSeek-V4-Pro**
- **Qwen3.8-Flash-Next**

所有 AI 生成或修改的代码均已由人工审核、实测验证通过后合入。
