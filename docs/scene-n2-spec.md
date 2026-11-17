# N2 交互态移植规格（three.js → C++ / Direct2D）

**定位**：本文是 `native-shell/docs/sphere-port-spec.md`（430 行，N1 + 大部分 N2 数值）的 **N2 交互态补全件**。只写该文档缺失或与源码不一致的部分，N1（粒子漂移 / 连线 / 静态相机）一律引用原规格，不重复。两份文档冲突时以本文 + 源码为准。

**行号基线**（2026-11-17 读取；所有行号均指下列文件当前版本，引用格式 `文件:行`）：

| 文件 | 行数 |
|---|---|
| Frontend/src/scene/Viewport.ts | 372 |
| Frontend/src/scene/SphereParticles.ts | 1106 |
| Frontend/src/scene/OriginSphere.ts | 93 |
| Frontend/src/scene/SphereLinks.ts | 227 |
| Frontend/src/scene/hexSphereSites.ts | 156 |
| Frontend/src/scene/easing.ts | 6 |
| Frontend/src/scene/AxisGizmo.ts | 100 |
| Frontend/src/scene/labelSprite.ts | 61 |
| Frontend/src/App.tsx | 57 |

**单位**：mm；球半径 `r = RADIUS = 2`（OriginSphere.ts:6）；粒子总数 **768**（SphereParticles.ts:32-39）。
**面向 C++/Direct2D 原生实现，不含任何 HTML/Web 技术要求。**

---

## 0. N2 常量速查（含行号）

### 0.1 Viewport.ts（相机 / 交互）

| 常量 | 值 | 行 |
|---|---|---|
| HOVER_FLY_TIME | 0.9 s | Viewport.ts:9 |
| HOVER_MIN_DISTANCE | 8 | Viewport.ts:17 |
| HOVER_MAX_DISTANCE | 14 | Viewport.ts:18 |
| CLICK_FLY_TIME | 1.2 s | Viewport.ts:21 |
| CLICK_SLOP | 5 px | Viewport.ts:24 |
| BACKGROUND_COLOR | 0x000000 | Viewport.ts:6 |
| 相机 | fov 50 / near 0.05 / far 2000，pos (9.5, 4.8, 11.5) | Viewport.ts:99-101 |
| controls | enableZoom=false、damping 0.08、rotate 0.65、zoom 0.9、pan 0.8、minDist 1.5、maxDist 120、maxPolar π/2−0.02、target (0,0,0) | Viewport.ts:103-113 |
| dt 上限 | 0.1 s | Viewport.ts:143 |

### 0.2 SphereParticles.ts（形变 / 点击态）

| 常量 | 值 | 行 |
|---|---|---|
| MORPH_DURATION / MORPH_STAGGER / MORPH_TOTAL | 0.75 / 0.35 / 1.1 s | :61 / :63 / :65 |
| TORUS_MAJOR_RATIO / TORUS_MINOR_RATIO | 0.42 / 0.12 | :68-69 |
| HEX_TUBE_RATIO / HEX_MITER_RATIO | 0.08 / 0.04 | :72 / :74 |
| HEX_SPIN_DEG_PER_SEC | −6 °/s | :76 |
| TORUS_TUMBLE_DEG_PER_SEC | 14 °/s | :78 |
| TORUS_AXIS_TILT / DRIFT_X / DRIFT_Y | 0.5 / 0.11 / 0.07 | :79-81 |
| SHAPE_TUBE_SPEED | 0.8 rad/s | :83 |
| TORUS_PATH_SPEED / HEX_PATH_SPEED | 0.45 rad/s / 0.4 mm/s | :85-86 |
| SHAPE_TORUS / SHAPE_HEX / SHAPE_MIDDLE | 1 / 2 / 3 | :89-91 |
| MIDDLE_INNER_RATIO / OUTER_RATIO / HEIGHT_RATIO | 0.575 / 0.75 / 0.125 | :94-97 |
| MIDDLE_PATH_SPEED | 0.3 rad/s | :99 |
| CLICK_SWAP_DURATION | 1.0 s | :102 |
| RING_MAJOR_RATIO / RING_MINOR_RATIO | 0.88 / 0.1015 | :115-116 |
| ASSEMBLY_SPIN_DEG_PER_SEC | 24 °/s | :123 |
| ASSEMBLY_AXIS_DRIFT_A / B / C | 0.09 / 0.13 / 0.07 | :124-126 |
| ASSEMBLY_SPIN_WAVE | 0.5 | :127 |
| RING_COUNT / RING_PARTICLES_PER_RING | 4 / 120（合计 480） | :135 / :137 |
| GIMBAL_RING_NORMALS | (1,1,1) (1,−1,−1) (−1,1,−1) (−1,−1,1) | :138-143 |
| BALL_RADIUS_RATIO | 0.25（Ø1 mm） | :158 |
| FOCUS_X_RAMP | 0.15 | :164 |
| BALL_PATH_SPEED / BALL_TUBE_SPEED | 0.45 / 0.35 rad/s | :176-177 |
| FOCUS_BLUR_GROW / FOCUS_BLUR_FADE | 2.0 / 0.85 | :184-185 |
| AIM_TAU（形态速度平滑时间常数） | 0.8 s | :50 |
| MIN_HOLD / MAX_HOLD（形态换向间隔） | 3 / 8 s | :46-47 |

---

## 1. N2 状态机完整伪码

### 1.1 状态变量

**Viewport 侧**：

| 变量 | 初值 | 行 | 含义 |
|---|---|---|---|
| hover | false | :64 | 悬停态（触发 morph） |
| flight | null | :65 | 进行中的相机飞行（含起止位姿 + elapsed + duration） |
| clickActive | false | :67 | 点击态（触发 clickSwap + 相机到 +X） |
| savedCameraPosition / savedTarget | (0,0,0) | :60-61 | 悬停前位姿，退出悬停时飞回 |
| savedClickPosition / savedClickTarget | (0,0,0) | :69-70 | 点击前位姿，倒放时飞回 |
| pointerDownAt | null | :72 | pointerdown 落点，用于 5px 拖拽判定 |
| onSwipeUp | 可选回调 | :78, :80-82 | 滚轮上滑 → 打开抽屉（§8.5） |

**SphereParticles 侧**：`morphActive`(:352)、`morphClock`(:353)、`clickActive`(:323)、`clickClock`(:324)。四个量只由 `setMorph`(:490-492)、`setClickSwap`(:528-532) 改开关，其余全在 `update` 里推进。

### 1.2 事件入口伪码

```
// ---- 指针移动（Viewport.ts:214-222）----
onPointerMove(x, y):
  if clickActive: return                                    // B1  :216  点击态期间 hover 不可改写
  hit = hitsBall(x, y)                                      // :218，射线拾取见 §8.2
  if hit && !hover && event.buttons == 0: enterHover()      // B2  :220  仅"未按键"时进入（拖拽中不打断）
  else if !hit && hover: exitHover()                        // B3  :221  退出任何时候允许

// ---- 指针离开画布（Viewport.ts:224-226）----
onPointerLeave():
  if hover && !clickActive: exitHover()                     // B4  :225

// ---- 按下（Viewport.ts:228-230）----
onPointerDown(x, y):
  pointerDownAt = {x, y}                                    // 无条件记录

// ---- 点击（Viewport.ts:233-245）----
onClick(x, y):
  down = pointerDownAt; pointerDownAt = null                // :234-235
  if down && hypot(x-down.x, y-down.y) > CLICK_SLOP(5): return   // B5 :237 拖拽不算点击
  if clickActive: reverseClick(); return                    // B6  :239-242  已在点击态 → 倒放
  if !hitsBall(x, y): return                                // B7  :243  未命中球壳 → 忽略
  enterClick()                                              // :244

// ---- 滚轮（Viewport.ts:248-257，监听 passive:false :129）----
onWheel(deltaY):
  if deltaY < 0: preventDefault(); onSwipeUp?.(); return    // B8  :249-253  上滑 → 开抽屉
  if deltaY <= 0 || !clickActive: return                    // B9  :254  此处只剩 deltaY>0；非点击态不动
  preventDefault(); reverseClick()                          // :255-256  下滑 → 点击态倒放
```

### 1.3 四个转换动作与 saved 位姿规则

```
// ---- 悬停进入（Viewport.ts:297-314）----
enterHover():
  if flight == null:                                        // B13 :299-302
    savedCameraPosition = camera.position
    savedTarget        = controls.target
  // flight != null（正飞回原位途中）→ 不覆盖 saved，保留最初位姿（注释 :298）
  hover = true                                               // :305
  setMorph(true)                                             // :306 → morphActive = true
  d = clamp(|camera.position − controls.target|, 8, 14)      // :308-312
  startFlight((0,0,d), (0,0,0), HOVER_FLY_TIME=0.9)          // :313

// ---- 悬停退出（Viewport.ts:316-321）----
exitHover():
  hover = false                                              // :318
  setMorph(false)                                            // :319 → morphActive = false
  startFlight(savedCameraPosition, savedTarget, 0.9)         // :320  from = 当前位姿（可能在半路）
  // 不改 saved

// ---- 点击进入（Viewport.ts:260-282）----
enterClick():
  if clickActive: return                                     // B10 :261  防重入
  clickActive = true                                         // :263
  if flight == null:                                         // B11 :266-268
    savedClickPosition = camera.position
    savedClickTarget   = controls.target
  else if hover:                                             // B12 :269-271
    savedClickPosition = savedCameraPosition   // 飞行半路点击 → 取悬停记下的最初位姿
    savedClickTarget   = savedTarget
  // else（flight!=null 且 hover==0，即"退场飞行中"）→ savedClick 不更新（:266-272 无第三分支）
  //   ⇒ 若从未点过，savedClick 仍是初值 (0,0,0)（:69-70）
  setClickSwap(true)                                         // :274 → clickActive = true（粒子侧）
  d = clamp(|camera.position − controls.target|, 8, 14)      // :276-280
  startFlight((d,0,0), (0,0,0), CLICK_FLY_TIME=1.2)          // :281
  // 注意：不清 hover —— 点击态可与悬停态并存；morphActive 也不动

// ---- 点击倒放（Viewport.ts:285-295，入口：再点一次 :239 或滚轮下滑 :256）----
reverseClick():
  if !clickActive: return                                    // B14 :286  防重入
  clickActive = false                                        // :288
  setClickSwap(false)                                        // :289  只动 click，不动 morph
  startFlight(savedClickPosition, savedClickTarget, 1.2)     // :290-294

// ---- 飞行启动（Viewport.ts:323-336）：无条件覆盖正在进行的 flight ----
startFlight(toPos, toTarget, duration=0.9):
  flight = { fromPosition: camera.position.clone(),   // from = 调用瞬间位姿（半路即半路）
             toPosition: toPos,
             fromTarget: controls.target.clone(),
             toTarget, elapsed: 0, duration }
```

### 1.4 updateCamera：`controls.enabled` 的每个分支（Viewport.ts:176-201）

```
updateCamera(dt):
  // B15 :178 —— 每帧重算；等价于"悬停 或 点击态 或 飞行中 ⇒ 禁用"
  controls.enabled = !hover && !clickActive && (flight == null)

  if flight:                                    // B16 :181-192
    flight.elapsed += dt                        // dt 已在 :143 钳到 ≤0.1
    t     = min(1, flight.elapsed / flight.duration)
    e     = easeInOutCubic(t)
    camera.position = lerpVectors(fromPosition, toPosition, e)
    controls.target = lerpVectors(fromTarget,   toTarget,   e)
    camera.lookAt(controls.target)
    if t >= 1: flight = null                    // :190（B16 的子判定）
    return

  if hover:                                     // B17 :194-198
    camera.lookAt(controls.target)              // 位置锁死，只保朝向
    return

  controls.update()                             // B18 :200（B17 的 else 分支）

```

真值说明（B15 的四种组合）：

| hover | clickActive | flight | controls.enabled | 实际效果 |
|---|---|---|---|---|
| 任意 | 任意 | 进行中 | false | 输入全禁，只跑 B16 插值 |
| true | 任意 | null | false | 位置锁死（B17） |
| false | true | null | false | 停在 +X；B18 仍调用 `update()`，但无输入增量（见下） |
| false | false | null | true | 正常拖拽旋转（B18） |

`enabled` 的作用面（three.js OrbitControls.js 事实）：只在输入事件处理器里检查 —— onPointerDown:1562、onPointerMove:1601、onMouseWheel:1785、onKeyDown:1799、onContextMenu:1955 各有 `if (this.enabled === false) return;`；`update()` 本身不检查 `enabled`。另 onMouseWheel:1785 还要求 `enableZoom !== false`，而 Viewport.ts:104 置 `enableZoom = false`，故 OrbitControls 永不消费滚轮、也不 preventDefault。

### 1.5 每帧时钟推进与钳制（SphereParticles.update，:639-668）

```
update(dt):
  if dt <= 0: return                                   // :640
  elapsed += dt                                        // :641（姿态旋转的时间轴，§3.6）

  // B19 :644-648 —— morphClock ∈ [0, 1.1]
  morphClock = morphActive ? min(MORPH_TOTAL=1.1, morphClock + dt)
                           : max(0,                 morphClock - dt)
  // B20 :651-655 —— clickClock ∈ [0, 1.0]
  clickClock = clickActive ? min(CLICK_SWAP_DURATION=1.0, clickClock + dt)
                           : max(0,                       clickClock - dt)

  simulate(dt)                                         // :657 自由漂移，恒运行（N1 §1.4）

  swap = (clickClock > 0)                              // :660
  if swap || morphClock > 0:                           // B21 :661-664
    updateShapeMotion(dt, swap)                        // :662 §3.2
    updateTargets(dt, swap)                            // :663 §3.3-3.6

  blendMorph()                                         // :666 §3.7
  syncBuffers()                                        // :667 §3.8 / §4
```

要点：
- 两个时钟**独立推进**：悬停未退出时进入点击态，`morphClock` 保持 1.1、`clickClock` 走 0→1.0；倒放只减 `clickClock`，不影响 `morphClock`（reverseClick 不碰 morph，Viewport.ts:289）。
- 钳制是对称的：正向 `min(满程, +dt)`、反向 `max(0, −dt)`，所以倒放顺序与进场完全一致（粒子按同一顺序飞回）。
- B21 门控：任一时钟 >0 就每帧重算目标点（**含退出途中**），两份目标（基准 + 点击态）并存，`blendMorph` 决定显示哪份。

### 1.6 分支清单（状态机共 21 支）

计数口径：**交互事件判定 + 相机锁定判定 + 时钟推进/门控判定**，每个 `if` / 条件表达式记 1 支；渲染插值内部的 `if`（blendMorph / updateTargets / simulate 等）不计入。B16 内的 `t >= 1`（:190）为其子判定，单列则为 22 支。

| # | 判定 | 位置 | 结果 |
|---|---|---|---|
| B1 | clickActive | Viewport.ts:216 | 返回，hover 不可改写 |
| B2 | hit && !hover && buttons==0 | :220 | enterHover |
| B3 | !hit && hover | :221 | exitHover |
| B4 | hover && !clickActive（pointerleave） | :225 | exitHover |
| B5 | down && 位移 > 5px（CLICK_SLOP） | :237 | 丢弃（拖拽） |
| B6 | clickActive | :239 | reverseClick |
| B7 | !hitsBall | :243 | 忽略 |
| B8 | deltaY < 0 | :249 | preventDefault + onSwipeUp |
| B9 | deltaY <= 0 || !clickActive | :254 | 剩余路径 = deltaY>0 且点击态 → reverseClick |
| B10 | clickActive（enterClick 重入） | :261 | 返回 |
| B11 | flight == null | :266 | savedClick ← 当前位姿 |
| B12 | else if hover | :269 | savedClick ← 悬停前位姿 |
| B13 | flight == null（enterHover） | :299 | saved ← 当前位姿 |
| B14 | !clickActive（reverseClick 重入） | :286 | 返回 |
| B15 | controls.enabled 布尔式 | :178 | 每帧锁定/放行 OrbitControls |
| B16 | flight != null | :181（含 :190 子判定） | 飞行插值后 return |
| B17 | hover | :194 | lookAt 后 return |
| B18 | else（B17 的另一支） | :200 | controls.update() |
| B19 | morphActive ? 正向 : 反向 | SphereParticles.ts:644 | morphClock 钳制 |
| B20 | clickActive ? 正向 : 反向 | :651 | clickClock 钳制 |
| B21 | swap || morphClock > 0 | :661 | 每帧重算形态/点击目标 |

---

## 2. 相机飞行的精确插值

### 2.1 结构与推进（Viewport.ts）

- `CameraFlight` 字段（:28-36）：fromPosition / toPosition / fromTarget / toTarget（只读）、elapsed、duration（只读）。
- `startFlight`（:323-336）：**无条件覆盖**现有 flight；`from` = 调用瞬间的 `camera.position` / `controls.target` 克隆；`elapsed = 0`。
- 每帧（:181-192）：

```
elapsed += dt                                   // dt = min(帧间隔, 0.1)（:143）
t     = min(1, elapsed / duration)
e     = easeInOutCubic(t)
camera.position.lerpVectors(fromPosition, toPosition, e)
controls.target.lerpVectors(fromTarget,   toTarget,   e)
camera.lookAt(controls.target)
if t >= 1: flight = null
```

- **缓动** easeInOutCubic（easing.ts:2-6）：`t<=0 → 0`；`t>=1 → 1`；`t<0.5 → 4t³`；否则 `1 − (2−2t)³/2`。
- **路径是直线**：position 直接线性插值，无球面/弧线约束，允许直穿球心（球心也在 target 上）。

### 2.2 四种飞行的目标位姿与时长

| 触发 | toPosition | toTarget | duration | from | saved 写入规则 |
|---|---|---|---|---|---|
| 悬停进入（:297-314） | **(0, 0, d)** | (0,0,0) | **0.9 s**（:9） | 当前位姿 | flight==null 才写（:299-302） |
| 悬停退出（:316-321） | savedCameraPosition | savedTarget | **0.9 s** | 当前位姿（可为半路） | 不写 |
| 点击进入（:260-282） | **(d, 0, 0)** | (0,0,0) | **1.2 s**（:21） | 当前位姿 | B11/B12，见 §1.3 |
| 点击倒放（:285-295） | savedClickPosition | savedClickTarget | **1.2 s** | 当前位姿（可为半路） | 不写 |

**d 的 clamp（两次进入各算一次）**：

```
d = clamp( |camera.position − controls.target| , HOVER_MIN_DISTANCE=8, HOVER_MAX_DISTANCE=14 )
    // 悬停进入 :308-312；点击进入 :276-280（同一表达式、同一常量 8~14）
```

即：用户当前距离落在 [8, 14] 内沿用原距离，否则收到边界上；`target` 在飞行中也在被插值，故"点击进入"的 d 取自**按下瞬间**的实时距离。

### 2.3 边缘情形（移植必须复刻）

1. **飞行可被覆盖**：进场途中移出 → `exitHover` 用 `startFlight` 覆盖，`from` = 半路位姿，时长仍是完整的 0.9 s（不是剩余时间）。
2. **enterHover 在飞行中不重写 saved**（:299-302，注释 :298 明说）：保证"飞回原位"途中再次进入悬停时，回程目标仍是最初位姿。
3. **enterClick 的第三种情形无分支**（:266-272）：`flight != null && hover == false`（正在退场飞行）时 `savedClick` 不更新 → 倒放会飞回上一次的 savedClick，初值为 (0,0,0)（:69-70）。
4. **点击不清 hover、不动 morph**（:260-282 全段未触碰 hover/morph）；`reverseClick` 同样只清 click（:285-295）。因此"悬停 → 点击 → 倒放"会回到悬停态：`hover=true`、`morphClock` 仍 1.1、相机飞回悬停位姿后由 B17 锁定。
5. **click 不检查 hover**（:233-245 只查 slop + 射线）：满足 B5/B7 即进入，可出现 `hover==false && clickActive==true`。
6. **clickActive 期间 hover 冻结**（B1）：pointermove 被挡、pointerleave 被 B4 挡，直到 reverseClick 后才恢复。
7. 飞行期间 `controls.enabled=false`（B15），相机只由 B16 控制；飞行结束后按 §1.4 真值表落到 B17 或 B18。

---

## 3. 每帧 updateTargets 的全部公式

### 3.1 每帧顺序（SphereParticles.update :639-668）

`update(dt)` → 时钟（§1.5）→ `simulate(dt)`（:657，N1 漂移恒跑）→ 若 B21 通过则 `updateShapeMotion(dt, swap)`(:662) + `updateTargets(dt, swap)`(:663) → `blendMorph()`(:666) → `syncBuffers()`(:667)。

### 3.2 updateShapeMotion（:671-722）—— 形态参数推进

```
decay     = 1 − exp(−dt / AIM_TAU=0.8)          // :672
loopLength = 6 * hexPathSpan() = 6 * 1.08r       // :673，hexPathSpan = r*(1+2*0.04)（:634-637）

对每颗粒子 i（:675-721）:
  shape = shapeKind[i]
  shapeHold[i] -= dt                              // :678
  if shapeHold[i] <= 0:                           // :679-692 每 3~8 s 重取目标速度
    shapeHold[i] = rand(3, 8)                     // :680
    pathMax = (shape==TORUS) ? 0.45
            : (shape==HEX)   ? (swap ? BALL_PATH_SPEED=0.45 : HEX_PATH_SPEED=0.4)
            :                 MIDDLE_PATH_SPEED=0.3          // :681-689
    shapePathAim[i] = rand(−pathMax, +pathMax)    // :689
    tubeMax = (swap && shape==HEX) ? BALL_TUBE_SPEED=0.35 : SHAPE_TUBE_SPEED=0.8  // :690
    shapeTubeAim[i] = rand(−tubeMax, +tubeMax)    // :691

  shapePathSpeed[i] += (aim − speed) * decay       // :694  指数平滑（同一 τ=0.8）
  shapeTubeSpeed[i] += (aim − speed) * decay       // :695

  if swap && shape == SHAPE_HEX:                  // :697-711 点击态：推进球面点参数
    ballPath[i] = wrapPeriod(ballPath[i] + shapePathSpeed[i]*dt, 2π)   // :699-702 φ 环向
    θ = ballTube[i] + shapeTubeSpeed[i]*dt                           // :703
    if θ < 0:    θ = −θ; shapeTubeSpeed[i] = |speed|                 // :704-706 极点反弹
    if θ > π:    θ = 2π−θ; shapeTubeSpeed[i] = −|speed|              // :707-710
    ballTube[i] = θ                                                   // :711
    // 该组的 shapePath/shapeTube 此帧**不推进（冻结）**
  else:                                            // :712-720 其余情况推进原生参数
    shapePath[i] += shapePathSpeed[i]*dt           // :713
    shapeTube[i] += shapeTubeSpeed[i]*dt           // :714
    pathPeriod = (shape==HEX) ? loopLength : 2π    // :717
    shapePath[i] = wrapPeriod(shapePath[i], pathPeriod)   // :718
    shapeTube[i] = wrapPeriod(shapeTube[i], 2π)           // :719
```

事实点：
- **swap 期间只有 SHAPE_HEX 走球面点分支**；甜甜圈/中间层（即"环组"里非征用的粒子）继续推进自己的 φ/θ，所以 4 只环在点击态仍在滚动。
- swap 期间六边形组的 `shapePath/shapeTube` 冻结，但其 `shapePathSpeed` 仍按 0.45 的上限被重新取向（:686），倒放结束后这些速度继续驱动原生参数。
- `ballPath/ballTube` 对全部 405 颗 SHAPE_HEX 粒子推进 —— 既包括 288 颗球面点，也包括 117 颗被征用粒子（它们把 ball 参数当环面参数用，§3.6）。

### 3.3 初值（构造时一次性）

`initMorphTargets`（:587-616）—— 每颗粒子：

| 量 | 公式 | 行 |
|---|---|---|
| morphDelay[i] | rand(0, 0.35) | :591 |
| shapeHold[i] | rand(3, 8) | :592 |
| shapeTubeAim[i] | rand(−0.8, 0.8) | :593 |
| shapeKind[i] | initialShape(i) | :595-596 |
| 甜甜圈 φ / θ | `[φ, θ] = sampleTorusAngles()` | :598-601 |
| 甜甜圈 pathAim | rand(−0.45, 0.45) | :602 |
| 六边形 θ | rand(0, 2π) | :605 |
| 六边形 path | rand(0, 6·pathSpan)（pathSpan=1.08r） | :606 |
| 六边形 pathAim | rand(−0.4, 0.4) | :607 |
| 中间层 path / tube | rand(0, 2π) / rand(0, 2π) | :610-611 |
| 中间层 pathAim | rand(−0.3, 0.3) | :612 |
| middleZ[i] | rand(−1, 1) · r · 0.125（固定） | :613 |

- `initialShape`（:220-225）：`local = i % 256`；`local<81 → TORUS`、`local<216 → HEX`、`else → MIDDLE` → 三组 **243 / 405 / 120**。
- `sampleTorusAngles`（:621-632，甜甜圈表面均匀采样，面积元 ∝ major + minor·cosθ）：

```
major = r*0.42 ; minor = r*0.12
tubeAngle = 0
for attempt in 0..7:                       // 最多 8 次
  u = rand(0, 2π)
  if major + minor*cos(u) >= rand(0, major+minor): tubeAngle = u; break
return [rand(0, 2π), tubeAngle]            // [主角 φ, 管角 θ]
```

- `initParticles`（:534-558）与 N1 相同（见原规格 §1.3）。

### 3.4 updateTargets 公共量（:728-783）

```
// 字段缺失防御（HMR 专用，:731-752）：shapeKind/shapePath/shapeTube/middleZ/target/ballPath/ballTube
// 任一缺失则跳过本帧并只报一次错 —— 原生移植可省。

radius  = r = 2
major   = r*0.42      ; minor   = r*0.12       // :755-756
tubeR   = r*0.08      ; side    = r            // :757-758（正六边形边长 = 外接圆半径）
miter   = r*0.04      ; pathSpan = side + 2*miter = 1.08r   // :759-760
middleInner = r*0.575 ; middleOuter = r*0.75                // :763-764
middleCenter = (inner+outer)/2 = 0.6625r ; middleAmp = 0.0875r  // :765-766

hexAngle   += degToRad(−6) * dt              // :769  六边形整体绕 Z 顺时针（从 +Z 看）
torusAngle += degToRad(14) * dt              // :772  甜甜圈翻滚角
axis = normalize( ( 0.5*sin(elapsed*0.11),
                    0.5*cos(elapsed*0.07),
                    1 ) )                    // :773-779  TORUS_AXIS_TILT=0.5, 0.11, 0.07
torusQuat = quat(axis, torusAngle)           // :780
cosHex = cos(hexAngle) ; sinHex = sin(hexAngle)   // :782-783
```

### 3.5 三组基准目标（每帧重算，:785-834）

**甜甜圈组（243 颗，shape==SHAPE_TORUS，:789-800）**
```
φ = shapePath[i] ; θ = shapeTube[i]
ring = major + minor*cos(θ)
local = ( ring*cosφ , ring*sinφ , minor*sinθ )
target = rotate(local, torusQuat)            // applyQuaternion(:796)
```

**六边形管组（405 颗，shape==SHAPE_HEX，:801-824）**
```
along = shapePath[i] ; θ = shapeTube[i]
edge  = floor(along / pathSpan) % 6                        // :805
t     = along − edge*pathSpan − miter ∈ [−miter, side+miter)   // :806
angleA = edge*π/3 ; angleB = angleA + π/3                  // :808-809
A = (r*cos angleA, r*sin angleA)                           // :810-811
dir = (B − A)/side                                         // :813-814（|B−A| = side）
n   = (−dir.y, dir.x)                                      // :815-816
local.x = A.x + dir.x*t + tubeR*cosθ*n.x                   // :818
local.y = A.y + dir.y*t + tubeR*cosθ*n.y                   // :819
local.z = tubeR*sinθ                                       // :820
target = ( local.x*cosHex − local.y*sinHex,
           local.x*sinHex + local.y*cosHex,
           local.z )                                       // :822-824  绕 Z 整体自转
```

**中间层（120 颗，:825-833）**
```
φ = shapePath[i]
ρ = middleCenter + middleAmp*sin(shapeTube[i])             // 半径在 0.575r~0.75r 呼吸
target = ( ρ*cosφ , ρ*sinφ , middleZ[i] )                   // z 固定，制造纵深
```

**早退**：`if (!swap) return`（:842）—— 非点击态不计算 clickTarget。

### 3.6 点击态目标（swap==true，:844-906）

**（a）整体随机自转（装配姿态，:844-860）**
```
e = elapsed
axis = normalize( ( sin(e*0.09), cos(e*0.13), 0.6 + 0.4*sin(e*0.07) ) )   // :848-854
spinMod = 1 + 0.5*sin(e*0.17)                                             // :855
q = quat(axis, degToRad(24) * spinMod * dt)                               // :856-859
gimbalOrientation.premultiply(q)                                          // :860  世界系累乘

// 4 只环的相对姿态锁死（:863-865）
ringQuaternion[k] = gimbalOrientation * gimbalFixedLocal[k]   k = 0..3
// gimbalFixedLocal[k] = quaternionFromZTo(GIMBAL_RING_NORMALS[k])，构造时算一次（:433-437）
```

`quaternionFromZTo(n)`（:147-157）：把局部 +Z 转到单位方向 n —— 轴 = Z×n = (−n.y, n.x, 0)，`|axis|² < 1e-12 → 单位四元数`；角 = `acos(clamp(n.z, −1, 1))`。4 条法线两两点积 = −1/3 → 环平面两两 70.53°。

**（b）被征用粒子与环分配（构造时一次，:442-464）**
```
wantRings = 4 * 120 = 480
nonHex    = #{shapeKind != HEX} = 243 + 120 = 363
extraNeeded = max(0, 480 − 363) = 117
isRing[i] = 1 若 shapeKind[i] != HEX            // 甜甜圈 243 + 中间层 120 全部
        否则若 takenFromHex < 117: 1, takenFromHex++   // 六边形组按 i 升序征用前 117 颗
rank = 0
对 i 升序: if isRing[i]: ringOf[i] = rank % 4 ; rank++      // 每环恰 120 颗
```
- 被征用的 117 颗 = **i 升序下最小的 117 个 SHAPE_HEX 下标 = i ∈ [81, 197]**（initialShape 使第一块 256 的 hex 为 i=81..215，取前 117 个）。
- 非环粒子 = 405 − 117 = **288 颗球面点**（ringOf = −1）。
- 数量校验：`initBallFromSites` 里若 `HEX_SPHERE_SITE_COUNT != hexCount` 只 `console.error` 一次（:567-572）；当前 initialShape 保证 hexCount=405 恒相等。

**（c）环上点（ringOf >= 0，:877-895）**
```
fromHex = (shape == SHAPE_HEX)                       // :880
φ = fromHex ? ballPath[i] : shapePath[i]             // :881
θ = fromHex ? ballTube[i] * 2 : shapeTube[i]         // :882  球极角 [0,π] → 管截面 [0,2π]
ringRadius = r*0.88 + r*0.1015*cos(θ)                // :867-868, :883
local = ( ringRadius*cosφ , ringRadius*sinφ , r*0.1015*sinθ )   // :885-890
clickTarget = rotate(local, ringQuaternion[ring])                  // :891, :893-895
```
- **被征用的六边形粒子**用球坐标参数顶上（φ 直接用、θ 用 2×球极角），与 288 颗球面点**共用同一套 ballPath/ballTube**（同为 SHAPE_HEX，同在 :697-711 被推进、同在 :566-585 被初始化）。
- 未征用的甜甜圈/中间层粒子沿用自己的 shapePath/shapeTube，且它们在 swap 期间继续推进（§3.2），故环持续滚动。

**（d）球面点（ringOf < 0，:896-905）**
```
φ = ballPath[i] ; θ = ballTube[i]
clickTarget = r*0.25 * ( sinθ*cosφ , sinθ*sinφ , cosθ )   // 半径恒 0.5mm = Ø1mm
```

### 3.7 blendMorph（显示坐标，:1003-1058）

```
clock = morphClock
swap  = clickClock / 1.0                      // :1004-1005

if clock <= 0 && swap <= 0:                   // :1008-1011 快路径
  position = simPosition（整段拷贝）; return

对每颗粒子 i（:1018-1057）:
  // 第 1 层：自由球面 ↔ 三组形态
  if clock <= 0:  (x,y,z) = simPosition[i]                       // :1025-1028
  else:
    p = easeInOutCubic( (clock − morphDelay[i]) / 0.75 )         // :1030
    if   p <= 0: (x,y,z) = simPosition[i]                        // :1031-1034
    elif p >= 1: (x,y,z) = target[i]                             // :1035-1038
    else:         (x,y,z) = lerp(simPosition[i], target[i], p)   // :1040-1043

  // 第 2 层：点击态覆盖（单段缓动，时钟倒走整段自然倒放）
  if swap > 0:                                               // :1047-1052
    t = easeInOutCubic(swap)
    (x,y,z) = lerp( (x,y,z), clickTarget[i], t )

  position[i] = (x,y,z)                                      // :1054-1056
```

### 3.8 syncBuffers（各层顶点 + focus，:1064-1094）

```
swapProgress = clickClock / 1.0                              // :1067
index = 0
对每层 layer（256 颗）:
  对层内 k:
    (px,py,pz) = position[(index+k)*3]                       // :1075-1079  显示坐标
    layer.positions[k*3 .. +2] = (px,py,pz)                  // :1080-1082
    xBlur = (px >= 0) ? 0 : min(1, −px / (r * FOCUS_X_RAMP=0.15))   // :1086
    layer.focus[k] = xBlur * swapProgress                    // :1087  （§4）
  index += layer.count ; attribute.needsUpdate = true        // :1090-1092
```

---

## 4. focus 虚化的精确公式与它影响的绘制参数

### 4.1 每颗 focus 值（唯一写入点，SphereParticles.ts:1086-1087）

```
xBlur(px) = 0                                  若 px ≥ 0（世界 x ≥ 0 一侧恒清晰）
          = min(1, −px / (r × 0.15))           若 px < 0    （FOCUS_X_RAMP = 0.15，:164）
focus[i]  = xBlur(px) × (clickClock / 1.0)     ← 仅点击态非零（:1067, :1087）
```

- `px` 是**显示坐标**（blendMorph 输出，:1077），不是目标坐标 → 虚化跟随当前实际渲染位置。
- 数值尺度：r=2 ⇒ 过渡带宽 `0.15r = 0.3 mm`；x = −0.3 mm 处 focus 到 1。
- 生效条件：`clickClock > 0`；非点击态 `swapProgress = 0` ⇒ focus 恒 0（含悬停态）。
- 数据通路：每层一个 `Float32Array(256)` + `BufferAttribute(itemSize=1)`（:366-368，DynamicDrawUsage），着色器 attribute 名 `aFocus`（:372）。

### 4.2 对绘制参数的影响（onBeforeCompile 补丁，:387-405）

```
// 顶点着色器（:388-395）
在 main() 前插入:  attribute float aFocus;  varying float vFocus;
在 #include <logdepthbuf_vertex> 处插入（即点大小/衰减计算之后）:
        vFocus = aFocus;
        gl_PointSize *= (1.0 + aFocus * 2.00);        // FOCUS_BLUR_GROW.toFixed(2) = "2.00"

// 片元着色器（:396-403）
在 main() 前插入:  varying float vFocus;
在 #include <color_fragment> 处插入:
        diffuseColor.a *= (1.0 - vFocus * 0.85);      // FOCUS_BLUR_FADE.toFixed(2) = "0.85"

material.customProgramCacheKey = 'focus-blur'          // :405
```

**受影响的绘制参数（仅此两项）**：

| 参数 | 公式 | 范围（focus ∈ [0,1]） |
|---|---|---|
| `gl_PointSize`（点大小） | × `1 + 2.0 × focus` | 1 → 3 倍 |
| `diffuseColor.a`（alpha） | × `1 − 0.85 × focus` | 1 → 0.15 倍 |

**不受影响**：RGB 颜色（恒白）、各层 `material.opacity`（0.5/0.8/1.0）、混合模式（加色）、`depthWrite=false`、`renderOrder=4`。合成后有效 alpha = `layerOpacity × texAlpha × (1 − 0.85×focus)`。
注：`gl_PointSize` 的基础值仍是原规格 §1.2 的 three.js 公式，本补丁在其上再乘放大系数。

### 4.3 相关源码注释勘误

- :1060-1063 注释写"按世界 **z** 切分、+z 半边实体"，代码按世界 **x**（:1086）。
- :161-164 注释写"**0.35r** 的过渡带"，实际 `FOCUS_X_RAMP = 0.15`（:164）。
- :179-183 注释写"失焦虚化（**常态**）：焦点在球心…"，实际 focus 恒乘 swapProgress，非点击态恒 0（:1087）。

---

## 5. hexSphereSites：生成算法与运行时用途

### 5.1 表结构

- `HEX_SPHERE_SITES`：**1215 个 float = 405 站 × (x, y, z)**，单位球面点，**4 位小数**字面量（hexSphereSites.ts:17-153）。
- `HEX_SPHERE_SITE_COUNT = HEX_SPHERE_SITES.length / 3 = 405`（:155-156）。
- 存的值**不精确归一**（4 位舍入），运行时用 `len = hypot(x,y,z)` 归一后再转球坐标（SphereParticles.ts:581-583）。

### 5.2 生成算法（文件头自述，hexSphereSites.ts:1-16；全部离线）

1. **纬度环密铺**（三角密铺）：每环点距 = `a`、环距 = `a·√3/2`、相邻环错开半格；`a` 由点数反推 **`a = √(4π / (N·√3/2))`**；各环点数**四舍五入**后把总数**补到 405**。
2. **球面弹簧松弛 8000 轮**：纯排斥、静止于目标点距、**每步投影回球面**，消掉纬度环之间的相位错位。
3. **实测指标**：六配位占比 76.8%（311/405）；最近邻距离 0.1727~0.1877（±4%）；其余为 5 配位缺陷，由欧拉公式决定（球面必然出现五边形缺陷）。

文件头未记录随机种子、步长、弹簧常数等细节（源码中也没有生成器，见 §5.4）。

### 5.3 运行时用途（唯一消费点）

- import：SphereParticles.ts:4；**只在构造函数调用一次** `initBallFromSites(hexCount)`（:469）。
- `initBallFromSites`（:566-585）：
  ```
  if count>0 && HEX_SPHERE_SITE_COUNT != count: console.error（只报一次，:567-572）
  rank = 0
  对 i 升序: 若 shapeKind[i] != SHAPE_HEX: continue
      s = (rank % 405) * 3 ; rank++
      (x,y,z) = HEX_SPHERE_SITES[s..s+2] ; len = hypot(x,y,z) || 1
      ballPath[i] = atan2(y/len, x/len)                 // φ  // :582
      ballTube[i] = acos(clamp(z/len, −1, 1))           // θ  // :583
  ```
- 之后**表不再被读取**：点击态的球面点只依赖 ballPath/ballTube（每帧被 §3.2 推进）。表的作用仅是**开局均摊**——注释 :13-15 明说"展示时六边形结构并不存在，点会立刻在球面上不规则游离，这里只保证开局不聚堆"。
- 只写 405 个 SHAPE_HEX 下标（含 117 颗被征用粒子）；当前 hexCount 恒 = 405，`rank % 405` 不会回绕。

### 5.4 C++ 是否需要这张表？—— 需要；且必须**离线烘焙**（事实依据）

| # | 事实 | 依据 |
|---|---|---|
| 1 | 文件头自述"生成与优化（**全部离线，运行时零开销**）" | hexSphereSites.ts:4 |
| 2 | 数据是**硬编码数组字面量**，模块加载即读入 | hexSphereSites.ts:17-153；SphereParticles.ts:4 |
| 3 | **仓库内没有生成脚本**：全仓库 grep `HEX_SPHERE_SITES`、`hexSphereSites`、`弹簧松弛` 只命中表文件本身、SphereParticles 的 import/注释、两份文档 | grep 结果 |
| 4 | 文件与仓库中**没有随机种子 / PRNG 记录** → 启动时重新生成无法复现这 405 个点 | 无任何种子常量 |
| 5 | 运行时只在构造函数读一次，无启动时重算的需求 | SphereParticles.ts:469 → :566-585 |

**结论**：C++ 侧把 **1215 个 float 作为静态常量表内嵌**（等价于原网页的离线烘焙），启动时生成既无必要、也无可复现依据。若自行重新生成，唯一视觉差异是点击态**开局一帧**的点位均摊（点随后立即不规则游离），但会与网页版不同步。

---

## 6. AxisGizmo / labelSprite 是否需要移植

**结论：未使用，无需移植。**

证据（grep 引用确认）：
1. grep `AxisGizmo|labelSprite|createLabelSprite`（Frontend/src）只命中两文件自身：AxisGizmo.ts:2（import）、:17（class 定义）、:42（内部使用）；labelSprite.ts:14（函数定义）。
2. 全仓库 grep `AxisGizmo` 只命中 AxisGizmo.ts:17 —— **Viewport.ts、OriginSphere.ts、App.tsx 均无引用**。
3. Viewport 构造只创建 `OriginSphere`（Viewport.ts:115-116）；`tick` 只 `renderer.render(scene, camera)`（:151），没有第二场景 / viewport / scissor 叠加渲染（AxisGizmo.render 需要，AxisGizmo.ts:60-83）。
4. OriginSphere.ts:14 注释明确"右下角坐标系已移除"。
5. 附带事实：labelSprite 依赖 `document.createElement('canvas')`（labelSprite.ts:22），本身是浏览器专用实现。

（若将来要恢复右下角坐标系指示器，才需要移植 AxisGizmo.ts + labelSprite.ts。）

---

## 7. 与 sphere-port-spec.md 的冲突 / 缺失勘误清单

### A. 源码过期注释（原规格未记录；移植一律以**代码**为准）

| 源码位置 | 过期/错误注释 | 代码事实 |
|---|---|---|
| SphereParticles.ts:104-114 | "甜甜圈组 → **两个圆环**，分别绕 X 轴 +45°/−45°"、"**三个环**…标准卡丹环" | `RING_COUNT = 4`（:135），正四面体 4 环，法线 (1,1,1)/(1,−1,−1)/(−1,1,−1)/(−1,−1,1)（:138-143） |
| :166-173 | "卡丹环**级联**：外环绕世界 X / 中环绕局部 Y / 内环绕局部 X，起手角 [0°,90°,90°]" | 无级联：4 环共用一个整体姿态 × 各自固定相对姿态（:863-865、:433-437） |
| :836-841 | 同上（"外环 = 甜甜圈A 绕世界 X…"） | 同上；环参数统一用 RING_MAJOR/MINOR（:867-868） |
| :499-501 | "第二形态（**莫比乌斯**）"、"断言**组内 90°**" | 第二状态 = 4 环 + 球面点；环间夹角 **70.53°**（法线点积 −1/3，:129-134 注释正确） |
| :161-164 | "0.35r 的过渡带" | `FOCUS_X_RAMP = 0.15`（:164） |
| :179-183 | "失焦虚化（**常态**）" | focus 恒 × swapProgress，非点击态恒 0（:1086-1087） |
| :1060-1063 | "按世界 **z** 切分 / +z 半边实体" | 按世界 **x** 切分（:1086） |
| :101 | "两图形直接互换，不打散、**相机不动**" | Viewport 把相机飞到 +X（Viewport.ts:281） |
| :107-108 | "Ø1mm 的球" 说明归属旧结构 | 数值正确（BALL_RADIUS_RATIO=0.25），但同段"两个圆环"描述已过期 |
| OriginSphere.ts:13 | 悬停态"外层**正六边形**" | 悬停态是**圆管**扫掠（HEX_TUBE_RATIO=0.08，SphereParticles.ts:801-824），非平面六边形 |
| Viewport.ts:43 | 悬停态"外层**双层**正六边形" | 代码为**单条**正六边形路径扫出的圆管，无"双层"结构 |
| SphereParticles.ts:6-7 vs :45/:474 | "768" 与 "648" 混用 | **768**（原规格 §勘误 已记录，仍有效） |
| SphereLinks.ts:21 | "648×12÷2=3888" | 768×12/2 = 4608（原规格已记录） |

### B. 原规格缺失（本文补全，对应章节）

| 缺失内容 | 本文 |
|---|---|
| N2 状态机完整伪码（hover/click/wheel 进出、重入保护、buttons==0 条件、clickActive 阻断） | §1.2-§1.3 |
| `controls.enabled` 的逐分支真值表 + enabled 的作用面 | §1.4 |
| morphClock/clickClock 推进与钳制、每帧门控 | §1.5 |
| saved 位姿的三种写入规则 + 初值 (0,0,0) + 边缘情形 | §1.3、§2.3 |
| startFlight 的"无条件覆盖 / from=半路位姿"规则 | §2.1、§2.3 |
| updateShapeMotion 在 swap 下的分支（hex 走球面点、θ 极点反弹、其余冻结/继续、速度上限切换 0.45/0.35） | §3.2 |
| 环分配算法（isRing/rank%4、被征用 117 颗的精确下标 i∈[81,197]、288 球面点） | §3.6(b) |
| 被征用粒子与球面点共用 ballPath/ballTube、θ=2×球极角 | §3.6(c) |
| blendMorph 的分支结构（快路径 / p 钳制 / swap 覆盖层） | §3.7 |
| wheel 的完整判定（方向映射、preventDefault、非点击态不动作） | §8.4 |
| `onSwipeUp` 语义（App.tsx → 打开抽屉） | §8.5 |
| AxisGizmo/labelSprite 结论 | §6 |
| hexSphereSites 的运行时用途细节、离线烘焙事实依据 | §5 |
| 源码过期注释勘误（上表 A） | §7A |

### C. 数值冲突核对

逐条比对原规格 §3.4、§4、§4.1、§4.2、§4.3、§5.2、§5.4 与源码 —— **未发现数值冲突**。已核对项：d clamp 8~14、0.9 s / 1.2 s、easeInOutCubic 分段、MORPH_TOTAL 1.1、CLICK_SWAP_DURATION 1.0、0.42/0.12、0.08/0.04、−6 °/s、14 °/s、0.5/0.11/0.07、0.575/0.75/0.6625/0.0875/0.125/0.3、0.45/0.4/0.8、0.88/0.1015、0.25、0.15、0.45/0.35、24 °/s、0.09/0.13/0.07/0.5、0.17、四面体法线、focus 公式、着色器 2.0/0.85。原规格 §4.2 的"环组 480 = 243+120+117、每环 120"也与 :442-464 一致（本文仅补充"哪 117 颗"的精确下标）。

---

## 8. wheel / pointer 事件的完整判定

### 8.1 监听注册（Viewport.ts:125-129）

| 事件 | 处理器 | 备注 |
|---|---|---|
| pointermove | onPointerMove | :125 |
| pointerleave | onPointerLeave | :126 |
| pointerdown | onPointerDown | :127 |
| click | onClick | :128 |
| wheel | onWheel，**`{ passive: false }`** | :129（故可 preventDefault） |

均挂在 `renderer.domElement`（画布）上；`dispose` 逐个移除（:353-357）。

### 8.2 拾取 hitsBall（Viewport.ts:204-212，悬停与点击共用）

```
rect = canvas.getBoundingClientRect()
ndc  = ( (x − rect.left)/rect.width  * 2 − 1 ,
        −(y − rect.top )/rect.height * 2 + 1 )          // :206-209
raycaster.setFromCamera(ndc, camera)                    // :210
return raycaster.intersectObject(originSphere.pickMesh).length > 0   // :211
```
- `pickMesh` = 不可见球壳：`SphereGeometry(2, 64, 48)` + `MeshBasicMaterial{0xffffff, opacity:0, depthWrite:false, DoubleSide}`、`renderOrder=1`（OriginSphere.ts:30-43）。
- 事实说明：three.js 对**三角化球面**求交，与解析球面差 ≤ 最大弦高。按 64 段（经向步长 5.625°）计算，`2×(1−cos 2.8125°) ≈ 0.0024 mm`（推导值）；C++ 用解析射线-球（r=2）求交即可，差异不可见。

### 8.3 CLICK_SLOP 与 dragging 判定

- **没有 `dragging` 标志**；拖拽由 OrbitControls 自行消费指针，场景只用位移阈值区分。
- `pointerdown` 记录 `pointerDownAt = {x,y}`（:229，无条件）。
- `click` 时：`down && hypot(Δx, Δy) > 5 px → return`（:237，`CLICK_SLOP = 5`，Viewport.ts:24）。
  - `down == null`（无 pointerdown 的 click）时**跳过** slop 检查，直接进入后续判定（`down &&` 短路）。
  - 阈值比较是严格大于（`> 5` 才算拖拽），位移恰为 5 px 仍算点击。
- 拖拽期间：`hover` 进入被 `event.buttons == 0` 挡住（:220），但**退出不受限**（:221）——拖拽旋转中把指针移出球会立刻退出悬停。
- 判定顺序（onClick）：slop → 已在点击态则 reverseClick → 射线未命中则忽略 → enterClick（:233-245）。

### 8.4 wheel 方向映射（Viewport.ts:248-257）

| 条件 | 动作 | preventDefault | 行 |
|---|---|---|---|
| `deltaY < 0`（上滑） | `onSwipeUp()` → 打开抽屉（§8.5） | 是 | :249-253 |
| `deltaY > 0` 且 `clickActive` | `reverseClick()`：点击态倒放 + 相机飞回（1.2 s） | 是 | :254-256 |
| `deltaY > 0` 且非点击态 | **无动作**（不 preventDefault、不缩放） | 否 | :254 |
| `deltaY == 0` | 无动作 | 否 | :254 |

补充事实：
- 缩放恒关闭：`controls.enableZoom = false`（Viewport.ts:104）；OrbitControls 的 onMouseWheel 因此直接 return（OrbitControls.js:1785 同时检查 `enabled` 与 `enableZoom`），滚轮只由 Viewport 的处理器解释。
- **无节流/防抖/累积**：每次 wheel 事件独立触发一次回调或一次倒放（上滑重复触发 `setOverlay('drawer')` 是幂等的）。
- 上滑**不清**任何场景状态；下滑**只在点击态**有效，非点击态的下滑完全无效果。

### 8.5 onSwipeUp 的语义（Frontend/src/App.tsx）

- Viewport 构造签名：`new Viewport(container, { onSwipeUp?: () => void })`（Viewport.ts:80-82）。
- App.tsx:18-20：`new Viewport(container, { onSwipeUp: () => setOverlay('drawer') })`。
- App.tsx:53：`<Drawer open={overlay === 'drawer'} onClose={() => setOverlay('none')} />`。
- **语义：滚轮上滑 = 打开"抽屉"浮层**；关闭由 Drawer 自己的 onClose 回调完成（App.tsx:53），与场景状态无关。
- 原生移植等价：滚轮上滑 → 打开抽屉/面板；不修改任何粒子、时钟、相机状态。
- 旁证（非 N2 范围）：App.tsx:29-48 有一个 400 ms 轮询 `/winkey-state` 的全局快捷键桥（Win 切任务界面），与本场景无关。

---

## 附：N2 状态机分支计数口径

- **21 支**（§1.6 B1–B21）：交互事件 9 支（B1–B9）+ 转换动作 5 支（B10–B14）+ 相机/controls 4 支（B15–B18）+ 时钟与门控 3 支（B19–B21）。
- 若把 B16 内的 `t >= 1`（Viewport.ts:190）单列 → **22 支**。
- 渲染插值内部判定（blendMorph 5 支、updateTargets 8 支、updateShapeMotion 5 支、simulate 3 支、syncBuffers 1 支等）不计入状态机，公式见 §3。
