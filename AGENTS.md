# Agent Instructions for ykkz000's DLL Plugin Loader

## 1. 项目概述

本项目是《Sid Meier's Civilization VI》（以下简写为“文明6”）的一个模组，提供 Loader DLL（`GameCore_YKKZ000_Loader_XP2`）。它作为原版 GameCore DLL（`GameCore_XP2_FinalRelease.dll`）的代理，加载原版 GameCore 并在此基础上应用插件的修改，同时发现并初始化各已启用模组提供的 ykkz000 DLL 插件。

- **技术栈**: C++, MSVC, Hook, CMake, vcpkg
- **架构**: DLL，游戏模组（无法独立运行，需要在游戏内加载）

## 2. 项目目录

项目目录为根项目目录下的`natives`目录

## 3. 测试

项目无法在脱离游戏本体的环境下运行，故无自动测试

## 4. 项目风格与规范

- 所有风格和规范**均不对第三方代码和文件生效**
- 所有注释使用美式英文

### 第三方依赖规范

- 第三方依赖**必须**支持`x86_64`（或`x64`、`amd64`等）框架、`win`（Windows）平台
- 第三方依赖的链接方式**必须**为静态链接
- 第三方依赖**优先**使用vcpkg管理依赖，若无法使用vcpkg管理依赖则将其放置于`third_party`目录
- **必须**尊重第三方依赖的许可协议，并按规定写入`THIRD_PARTY_NOTICES.txt`

### C++ 代码风格与规范

- 对于由头文件定义的对外暴露的函数和类型需要使用Doxygen注释
- 对于类中`public`和`protected`成员变量和成员方法需要使用Doxygen注释
- 代码应当解耦，不应当出现属于特定模块的逻辑的代码出现在其它模块
- 所有日志输出使用美式英文
- 编译的二进制文件始终携带Copyright信息和许可协议信息（许可协议同项目许可协议，仅提供许可协议名即可）

#### 宏规范

- **优先**使用C++预定义宏、编译器预定义宏和业界常用宏
- 对于由头文件定义的作为API的宏统一使用`YKKZ000_`前缀，而在编译时传入的宏则不使用`YKKZ000_`前缀

#### 日志规范

- 日志统一使用log4cxx
- 日志文件名**禁止**携带时间戳等信息，且日志文件**必须**在每次运行时清空

#### 公共头文件规范

- 所有项目定义的类型和结构均在`ykkz000`命名空间
- 单一头文件**只能**涉及一个命名空间内的函数、类型等定义
- 所有公共头文件的路径结构**必须**与命名空间结构相同

##### Civ6数据结构规范

- 定义：Civ6数据结构指在游戏的GameCore DLL中定义和使用的数据结构
- Civ6数据结构**必须**位于`ykkz000::civ6`命名空间中

#### Shared ABI headers

- `natives/include/ykkz000/bridge/**`、`export.h` 与 `natives/include/ykkz000/civ6/**` 是本仓库与其他模组共享的 ABI 头文件。**本仓库是这些头文件的权威源**；其它仓库（Plugin API、consumer）内置的是副本，变更时需同步。
- `bridge/host.h` 中的 `PluginManifest`、`Host` 等结构遵循**只增不改**策略：只允许追加字段，已发布字段的含义**禁止**更改。

#### Loader 代码规范

- Loader**必须**始终保持与游戏的GameCore DLL在游戏中被调用时的兼容性
- Loader**必须**加载游戏的GameCore DLL，并在此基础上应用插件的修改，**禁止**直接替代游戏引擎
- Loader**禁止**实现与修改游戏玩法相关的逻辑，与游戏玩法相关的逻辑均由插件修改
- Loader 仅从**已启用**模组中发现插件；跨模组发现依赖引擎提供的 enabled-mod 列表，列表不可用时仅扫描 Loader 自身目录

### 插件契约（Loader 侧）

- 插件**必须**导出`GetPlugin`/`DestroyPlugin`/`GetPluginManifest`，并在清单中声明唯一逻辑名`name`、依赖数组`dependencies`与最低 Host API 版本`requiredHostApi`
- Loader 校验缺失依赖、重复名称、依赖环与`requiredHostApi`，并在初始化前按依赖拓扑排序
- 初始化遵循**两阶段**：先加载全部候选DLL，再按拓扑序调用`GetPlugin`；卸载按**逆拓扑序**

## 5. Git 规则

- **提交信息**: 遵循 Conventional Commits
- **提交控制**：仅暂存流程中的修改的文件，提交后上传到远程仓库
