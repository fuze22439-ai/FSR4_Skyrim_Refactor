# FSR4_Skyrim_Refactor

AMD FSR 4.0 Frame Generation implementation for Skyrim Special Edition.

为《上古卷轴5：特别版》实现的 AMD FSR 4.0 帧生成插件。采用 Proxy SwapChain 架构处理 D3D11 到 D3D12 的互操作。目前处于重构与优化阶段，旨在提供原生级的插帧体验。

## 核心特性 (Core Features)
- **FSR 4.0 原生支持**: 利用最新的 FidelityFX SDK 实现高质量插帧。
- **Proxy SwapChain 架构**: 通过代理交换链技术，在 D3D11 游戏引擎中注入 D3D12 渲染管线。
- **反向互操作 (Reverse Interop)**: 采用最稳定的 D3D12 资源共享方案，确保与 ENB 等插件的兼容性。
- **精准同步**: 严格同步 FrameID 与相机抖动 (Jitter)，消除画面撕裂与拖影。

## 当前进度 (Current Progress)
- [x] 基础 Proxy SwapChain 搭建
- [x] FSR 4.0 上下文初始化
- [x] 相机向量 (Camera Vectors) 映射修复
- [x] Jitter 符号与缩放校正
- [x] 资源屏障 (Resource Barriers) 优化
- [ ] **进行中**: 解决插帧无效/静默回退问题
- [ ] **进行中**: 运动矢量 (Motion Vector) 极性微调

## 技术细节 (Technical Details)
- **开发环境**: Visual Studio 2022, CMake, Vcpkg
- **依赖库**: 
  - AMD FidelityFX SDK (FSR 4.0)
  - CommonLibSSE-NG
  - Detours
- **架构方案**: 
  - 使用 **SKSE::PatchIAT** 挂钩 `D3D11CreateDeviceAndSwapChain`。
  - 使用 **Detours VTable Hook** 挂钩 `IDXGIFactory::CreateSwapChain` 以注入代理交换链。
  - 采用 **Reverse Interop** (D3D12 创建资源 -> D3D11 打开共享句柄)。

## 致谢 (Credits)
参考并借鉴了以下项目的优秀实现：
- [ENBFrameGeneration](https://github.com/PureDark/ENBFrameGeneration)
- [enb-anti-aliasing](https://github.com/PureDark/enb-anti-aliasing)
- [Skyrim-Upscaler](https://github.com/PureDark/Skyrim-Upscaler)

## 免责声明 (Disclaimer)
本项目仅用于技术研究与交流。请确保你拥有合法的游戏副本。
