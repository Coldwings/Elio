# Elio v0.9.0 Release Readiness — Final Consensus

> Generated: 2026-06-09
> Source: 5-faction code debate, 3 rounds, full consensus reached
> Factions: Architecture Guardian, Performance Engineer, API Advocate, Reliability Engineer, Simplicity Advocate

---

## Executive Summary

Elio **尚未准备好 v1.0**，但核心架构和 API 已接近 release 就绪。
建议版本路径：`0.3.0` → 修复 P0 → `v0.9.0-rc1` → 修复 P1 → `v0.9.0` → 生产验证 → `v1.0.0`

当前状态：99 个头文件，~32K 行代码，1326+ 测试用例（含 ASAN/TSAN），112 个 commit，0 个 release tag。

---

## P0 — Release Blocker（必须在 v0.9.0-rc1 前完成）

### 1. 修复 semaphore::release() permit 泄漏

**文件**: `include/elio/sync/primitives.hpp:662`
**问题**: `release(n)` 唤醒 `to_wake` 个等待者后，`count_ += count` 应为 `count_ += (count - to_wake)`。被唤醒的 waiter 在 `await_resume()` 中不消耗 permit（no-op），导致多发放 `to_wake` 个幽灵 permit。
**影响**: semaphore 保护的资源可被超过上限并发访问。
**修复**: 一行代码变更 + 回归测试（先写测试验证 bug 存在，再修复）。
**共识**: 5/5 流派独立验证确认。

### 2. 补齐 elio::go_to() 自由函数

**文件**: `include/elio/runtime/spawn.hpp`
**问题**: `elio::go()` 和 `elio::spawn()` 有顶级自由函数，但 `go_to(worker_id, ...)` 仅存在于 `scheduler::go_to()` 方法。用户想 pin 到特定 worker 必须手动获取 scheduler 指针。
**修复**: 在 `spawn.hpp` 中添加 `elio::go_to(size_t worker_id, F&&, Args&&...)`，与 `elio::go()` 对称。
**共识**: 5/5 流派一致。

### 3. 修复 when_all 单参数返回类型突变

**文件**: `include/elio/coro/when_all.hpp:127-136`
**问题**: `sizeof...(Fs) == 1` 时返回裸值 `T`，多参数时返回 `std::tuple<Ts...>`。泛型代码无法统一处理。
**修复**: 始终返回 `std::tuple`（单元素 tuple 在 structured binding 下零成本）。
**共识**: 5/5 流派一致。

### 4. channel 改为 non-movable

**文件**: `include/elio/sync/primitives.hpp:778-786`
**问题**: move 构造器不持任何 mutex，与并发 send/recv 存在 data race (UB)。channel 作为共享同步原语，move 语义与其设计意图不匹配。
**修复**: `channel(channel&&) = delete; channel& operator=(channel&&) = delete;`，与 mutex/semaphore/event 一致。
**共识**: 5/5 流派一致（non-movable 是最简约的正确方案）。

### 5. 创建 CHANGELOG.md

**问题**: 112 个 commit，0 个 release tag，无变更历史记录。任何形式的 release 都需要 CHANGELOG。
**修复**: 从 git log 回溯整理，至少覆盖主要里程碑（0.1 → 0.2 → 0.3 → 0.9）。
**共识**: 5/5 流派一致。

---

## P1 — 应在 v0.9.0 正式版前完成

### 6. channel(0) 语义明确化

**文件**: `include/elio/sync/primitives.hpp:765-766`
**问题**: `capacity = 0` 意味着 unbounded（注释标注），但 Go 等语言中 channel(0) 是 rendezvous 语义。unbounded 默认值是 production OOM 的经典来源。
**方案**: 文档明确标注 + 考虑引入 `channel<T>::unbounded()` 工厂方法或删除默认参数强制显式声明。

### 7. ELIO_ASYNC_MAIN 4 变体合并

**文件**: `include/elio/runtime/async_main.hpp:277-309`
**问题**: `ELIO_ASYNC_MAIN`, `ELIO_ASYNC_MAIN_VOID`, `ELIO_ASYNC_MAIN_NOARGS`, `ELIO_ASYNC_MAIN_VOID_NOARGS` 四个宏是笛卡尔积爆炸。
**方案**: 统一为 1 个 `ELIO_ASYNC_MAIN(func)` 宏，内部用 `if constexpr` + concepts 推导签名。其余标记 deprecated。

### 8. TSAN 测试恢复 sync 原语多线程运行

**文件**: `tests/unit/test_sync.cpp`（至少 9 处注释 "Use single-threaded scheduler to avoid TSAN memory reuse"）
**问题**: sync 原语的多线程竞争场景未被 TSAN 实际验证。semaphore bug 的存在侧面印证了这一盲区的后果。
**方案**: 逐步恢复多线程 TSAN 测试，修复可能暴露的竞争问题。

### 9. vthread_stack assert 替换

**文件**: `include/elio/coro/vthread_stack.hpp:117-120`
**问题**: `assert()` 在 release build (NDEBUG) 中消失，导致静默内存损坏。
**方案**: 替换为 `ELIO_VERIFY` 或 `[[unlikely]] if (...) __builtin_trap()`。

### 10. README 版本信号修正

**问题**: README 自称 "production-ready" 但版本号 0.3.0，信号矛盾。
**方案**: 替换为 "approaching stable release" 或类似措辞，待 v1.0 后再标 production-ready。

### 11. 统一 io_context 移动语义

**文件**: `include/elio/io/io_context.hpp:70-76`
**问题**: io_uring 后端下不可移动，epoll 后端下可移动。同一代码在不同编译配置下行为不同。
**方案**: 统一为 non-movable（io_context 绑定 OS 资源，移动语义无价值）。

### 12. 建立 benchmark baseline

**问题**: 缺少任何形式的性能基准记录。
**方案**: 确保现有 benchmark binary 可运行，记录 v0.9.0 的 spawn/yield/channel/io 基准数据写入 release notes。不要求 CI 集成。

---

## 已达成共识的设计决定

### go() 与 spawn() 的错误处理

**决定**: 保持当前行为差异，**这是设计意图而非 bug**。

- `go()` 无 scheduler 时 **abort**（fire-and-forget 无返回通道，异常传不回去，abort + 诊断信息是最诚实的失败模式）
- `spawn()` 无 scheduler 时返回带 `logic_error` 的 join_handle（joinable 语义下有异常传播通道）

**共识**: 4/5 流派一致（Arch/API/Rel/Sim），Perf 让步但不反对。
**文档**: 应在 API 文档中明确说明这一有意的差异及其理由。

### Stability Tier 分级体系

| Tier | 模块 | API 承诺 |
|------|------|----------|
| **Stable** | coro (task/generator/cancel_token/when_all), runtime (scheduler/spawn/async_main/autoscaler), io, net (tcp/uds), sync, time, signal, tls | 0.9.x 内不做 breaking change |
| **Provisional** | http, rpc, log, hash/crc32 | 基本稳定，minor 版本可能调整签名 |
| **Experimental** | when_any, rdma 全家族, hash/sha* | 可能随时变更，需 ELIO_EXPERIMENTAL 或独立 CMake 开关 |

**共识**: 5/5 流派一致。

### when_any 保持 EXPERIMENTAL

- 取消语义为 cooperative cancellation，无法中断进行中的 I/O
- loser task 会继续运行直到下一次 cancel_token 检查点
- 高扇出场景（如探测 10 个副本取最快）会临时泄漏 9 个 coroutine frame
- 头文件保留 ELIO_EXPERIMENTAL 门控，不进入 umbrella header
- v1.0 前需要验证取消语义或重新设计

### sync 原语实现策略

- mutex 和 shared_mutex 读路径已是 lock-free（验证通过）
- semaphore/event/channel/condition_variable 当前使用 std::mutex + std::queue
- **不在 v0.9.0 周期内进行 lock-free 统一重写**（高风险，复杂度 3-5 倍）
- 标记 Provisional，在 v1.x 中根据 benchmark 数据按需逐个优化
- **共识**: 5/5 流派一致。

### primitives.hpp 拆分

- 当前 1441 行单文件包含 7 个同步原语类型
- **推迟到 v1.x**，不阻塞 release
- 拆分时保留 `primitives.hpp` 作为聚合头（向后兼容）
- **共识**: 5/5 流派一致。

### HTTP/RPC/Hash 模块定位

- 代码保留在仓库中，通过 CMake 开关 + Stability Tier 标记管理
- 不需要移出仓库或物理隔离
- HTTP/RPC 标记为 Provisional，RDMA/hash-sha 标记为 Experimental
- **共识**: 5/5 流派一致。

---

## P2 — v1.x 系列跟进（不阻塞 release）

| # | 项目 | 来源 |
|---|------|------|
| 13 | when_all/when_any 解耦 coro/ 对 runtime/ 的反向依赖 | Arch |
| 14 | sync/primitives.hpp 拆分为独立头文件 | Arch + API |
| 15 | spawn 热路径堆分配优化（vthread_stack pool, join_state intrusive refcount） | Perf |
| 16 | io_uring poll() deferred_resumes vector 复用 | Perf |
| 17 | 建立 benchmark 回归 CI（spawn/channel/io 指标，10% 退化阈值） | Perf |
| 18 | 删除 hash/sha1.hpp 和 hash/sha256.hpp（零内部使用者，WebSocket 走 OpenSSL） | Sim |
| 19 | benchmark 示例移出 examples/ 到 benchmarks/ | Sim |
| 20 | CMake SameMajorVersion → SameMinorVersion（0.x 下语义修正） | Arch |
| 21 | condition_variable::wait(mutex&) 从 task<void> 重构为 awaitable | API + Perf |
| 22 | when_any 返回类型从 pair{index, variant} 改为结构化类型 | API |

---

## Version Roadmap

```
0.3.0 (current)
  │
  ├── Fix P0 items (5 items)
  │
  v
0.9.0-rc1
  │
  ├── Fix P1 items (7 items)
  ├── CHANGELOG.md
  ├── Stability tier documentation
  │
  v
0.9.0 (first tagged release)
  │
  ├── Production validation period
  ├── P2 items (as needed)
  ├── User feedback incorporation
  │
  v
1.0.0 (stable release)
```
