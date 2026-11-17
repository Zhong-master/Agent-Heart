// ============================================================
// sphere_scene.h — N1 球体场景原生实现 (task-8)
//
// 权威规格: docs/sphere-port-spec.md (430 行, 6 章), 逐项对照实现:
//   §1 粒子 768 颗 (三层各 256; 勘误: 不是 648) 恒定球面漂移
//   §2 连线 K=12 近邻暴力重算, MAX_EDGES=8192, 背向压暗 + 距离衰减
//   §3 相机 fov=50 / near 0.05 / 初始位 (9.5,4.8,11.5) 看原点 (静态)
//   §5 帧循环 dt 上限 0.1s; §6.1 N1 必须移植集合
//
// 渲染管线 (零第三方, 纯 Win32 + D2D + Composition):
//   D3D11(硬件→WARP) → IDXGIDevice → D2D1CreateDevice
//   → ICompositorInterop::CreateGraphicsDevice → CompositionDrawingSurface
//     (B8G8R8A8_UNORM / premultiplied)
//   每帧 ICompositionDrawingSurfaceInterop::BeginDraw 取 ID2D1DeviceContext:
//     Clear(纯黑) → 连线 DrawLine → 粒子 FillEllipse → EndDraw
//   D2D1_PRIMITIVE_BLEND_ADD 实现 three.js AdditiveBlending (加色混合)
//   SpriteVisual + CompositionSurfaceBrush(Stretch::Fill) 挂 root_ 最上层
// ============================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
// Composition 图形设备 / 绘制表面 interop (ICompositorInterop, ICompositionDrawingSurfaceInterop)
#include <windows.ui.composition.interop.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <winrt/Windows.UI.Composition.h>

#include <array>
#include <functional>
#include <random>
#include <vector>

// 768 粒子球体场景: 漂移模拟 + K=12 近邻连线 + D2D 加色绘制到 Composition 表面。
// 所有方法只在 UI 线程 (DispatcherQueue 所在线程) 调用, 无需加锁。
class SphereScene {
public:
    // 创建 D2D/Composition 图形设备、视觉体与 768 粒子状态。
    // w/h > 0 时顺带建立绘制表面; 传 0 则由随后的 Resize() 建立 (BuildScene 即此路径)。
    bool Init(winrt::Windows::UI::Composition::Compositor const& compositor, float w, float h);

    // 像素密度 = min(devicePixelRatio, 2) 的底数: 传 GetDpiForWindow(hwnd)/96。
    // (超出规格 4 个方法之外的补充: main 的 Layout 在 WM_SIZE 里知道窗口 DPI,
    //  而 Init/Resize 签名按任务约定不含 HWND, 故单独同步。)
    void SetDpiScale(float scale);

    // 窗口尺寸变化 (布局单位, 与 root_.Size 同单位): 同步视觉体尺寸与表面像素尺寸。
    void Resize(float w, float h);

    // 每帧驱动: 漂移模拟 → 近邻/连线重算 → 绘制。dt 单位秒, 上限由调用方钳到 0.1 (规格 §5.1)。
    void Tick(double dt);

    // 挂到 root_ 最上层的全屏不透明黑底表面视觉体。
    winrt::Windows::UI::Composition::SpriteVisual Visual() const { return visual_; }

    // 诊断计数: Tick 被调次数 / BeginDraw+EndDraw 成功帧数 (main 在 60 次时打日志)。
    int TickCount() const noexcept { return ticks_; }
    int FrameCount() const noexcept { return frames_; }

    // ================= N2 交互 API (docs/scene-n2-spec.md) =================
    // 坐标 = 客户区 DIP (与 Resize 的 w/h 同单位); 屏幕像素->DIP 由调用方换算。
    void PointerMove(float x, float y, bool buttonsDown);  // 悬停射线拾取 + 拖拽旋转
    void PointerDown(float x, float y);                    // 记录起点 (CLICK_SLOP=5px)
    void PointerUp(float x, float y);                      // 位移<=5px 判 click -> 切换点击态
    void Wheel(int deltaY);                                // >0 且点击态 -> reverseClick(); <0 开抽屉由 main 负责
    bool ClickActive() const;                              // 点击互换态 (main 滚轮路由依赖)
    bool HoverActive() const;                              // 悬停形变态
    // 自测辅助: 读出相机位置 (拖拽旋转前后对比); 返回客户区无关的世界坐标
    void CameraPosition(float& x, float& y, float& z) const {
        x = camPos_.x; y = camPos_.y; z = camPos_.z;
    }

    // Stage3 覆盖层钩子: 场景画完之后、EndDraw 之前调用; dc 已带 DPI 变换 (单位 DIP),
    // 混合模式已切到 SOURCE_OVER; sceneSnapshot 为场景快照位图 (毛玻璃源), 仅快照开启时非空。
    using OverlayDrawFn = std::function<void(ID2D1DeviceContext*, ID2D1Bitmap*, float dipW, float dipH)>;
    void SetOverlayDraw(OverlayDrawFn fn) { overlayDraw_ = std::move(fn); }
    void SetSnapshotEnabled(bool on) { snapshotEnabled_ = on; }

private:
    // ---- 规格常量 ----
    static constexpr int kParticles = 768;   // PARTICLE_COUNT = 3 层 × 256 (勘误: 非 648)
    static constexpr int kPerLayer = 256;    // 每层粒子数
    static constexpr int kNearest = 12;      // NEAREST_COUNT (K 近邻)
    static constexpr int kMaxEdges = 8192;   // MAX_EDGES 连线容量硬顶
    static constexpr float kRadius = 2.0f;   // RADIUS 球半径 (mm)

    // ---- 小向量 (POD + 四则) ----
    struct V3 {
        float x, y, z;
        V3 operator+(V3 o) const { return { x + o.x, y + o.y, z + o.z }; }
        V3 operator-(V3 o) const { return { x - o.x, y - o.y, z - o.z }; }
        V3 operator*(float s) const { return { x * s, y * s, z * s }; }
    };

    // §1.3 每颗粒子状态 (自由漂移; N1 无形变, display = simPosition)
    struct Particle {
        V3 pos;    // simPosition: 恒在半径 2 球面
        V3 dir;    // direction: 当前朝向 (切向单位向量)
        V3 aim;    // aim: 目标朝向 (3~8s 换向 ±110°)
        float speed;   // mm/s, [0.12, 0.42]
        float hold;    // 换向倒计时 s, [3, 8]
        float phase;   // 正弦调制初相 [0, 2π]
        float pulse;   // 正弦调制角频率 rad/s, [0.2, 0.7]
    };

    // §2 一条候选连线, fade 为距离衰减亮度
    struct Edge {
        int i, j;
        float fade;
    };

    // ---- 初始化 / 模拟 (规格 §1) ----
    void InitParticles();            // §1.3 初始分布
    void Simulate(double dt);        // §1.4 每帧漂移
    void UpdateLinks();              // §2.3 facing + K 近邻 + 去重连线
    void Draw();                     // §1.2 投影 + D2D 绘制
    bool CreateSurface(int pxW, int pxH);

    // ---- 随机 (规格 §1.3) ----
    float Rand();                    // [0,1)
    float RandBetween(float a, float b);
    float Gauss();                   // Box-Muller 标准正态
    V3 RandomUnitVector();           // 均匀球面方向, 最多 8 次重试
    V3 RandomTangent(V3 p);          // 切向量 (正确切平面投影, 规格允许)

    // ---- K 近邻 (规格 §2.1) ----
    void InsertNeighbor(int i, int j, float distSq);  // 平方距离升序插入
    bool HasNeighbor(int i, int j) const;

    // ---- 向量工具 ----
    static float Dot(V3 a, V3 b);
    static V3 Cross(V3 a, V3 b);
    static float Length(V3 a);
    static V3 Normalize(V3 a);
    static V3 RotateAxis(V3 v, V3 axis, float angle);  // Rodrigues 旋转

    // ---- composition / d2d 资源 ----
    winrt::Windows::UI::Composition::Compositor compositor_{ nullptr };
    winrt::Windows::UI::Composition::SpriteVisual visual_{ nullptr };
    winrt::Windows::UI::Composition::CompositionDrawingSurface surface_{ nullptr };
    winrt::Windows::UI::Composition::CompositionSurfaceBrush brush_{ nullptr };
    winrt::Windows::UI::Composition::CompositionGraphicsDevice graphicsDevice_{ nullptr };
    winrt::com_ptr<ID2D1Device> d2dDevice_;

    // ---- 尺寸 / 状态 ----
    bool initialized_ = false;
    float dpiScale_ = 1.0f;             // min(devicePixelRatio, 2) 底数
    int pxW_ = 0, pxH_ = 0;             // 表面像素尺寸 (0 = 暂停绘制, 如最小化)
    float aspect_ = 1.0f;               // 宽高比 (投影用)
    float f_ = 2.1445069f;              // 1/tan(fov/2) = 1/tan(25°), Init 里按 §3.1 重算
    double elapsed_ = 0.0;              // 累计时间 (正弦调制用)
    int ticks_ = 0, frames_ = 0;        // 诊断计数

    // ---- 粒子与连线状态 (全部堆上, 避免撑大栈) ----
    std::vector<Particle> particles_;                      // 768
    std::vector<float> facing_;                            // 768 §2.3 步骤1
    std::vector<std::array<int, kNearest>> nbrIdx_;        // 768×12 近邻索引
    std::vector<std::array<float, kNearest>> nbrD2_;       // 768×12 平方距离 (升序)
    std::vector<int> kn_;                                  // 768 已填近邻数
    std::vector<float> farthest_;                          // 768 sqrt(第12名平方距离)
    std::vector<Edge> edges_;                              // ≤8192
    int edgeCount_ = 0;
    std::vector<float> sx_, sy_;                           // 屏幕坐标 (设备像素)
    std::vector<float> depth_;                             // 视深 -p_v.z
    std::vector<float> psize_;                             // 点大小 (设备像素, §1.2)

    // ---- 随机数发生器 (固定种子, 便于复现) ----
    std::mt19937 rng_{ 0x5EEDu };

    // ---- Stage3 覆盖层钩子 / 场景快照 ----
    OverlayDrawFn overlayDraw_;
    bool snapshotEnabled_ = false;
    winrt::com_ptr<ID2D1Bitmap1> snapshotBmp_;
    bool EnsureSnapshot(ID2D1DeviceContext* dc);

    // ================= N2 私有状态 (docs/scene-n2-spec.md) =================
    // ---- 四元数 (点击态环装配姿态, §3.6) ----
    struct Quat {
        float x, y, z, w;
    };
    static Quat QuatAxisAngle(V3 axis, float angle);
    static Quat QuatMul(Quat a, Quat b);          // a * b (Hamilton 积)
    static V3 QuatRotate(Quat q, V3 v);           // v' = q v q⁻¹
    static Quat QuatFromZTo(V3 n);                // 局部 +Z → n (§3.6a)

    // ---- 相机 / OrbitControls (§1.4 / §2 / §3.2) ----
    V3 camPos_ = { 9.5f, 4.8f, 11.5f };           // 初始位 (fov 50 / near 0.05 / far 2000)
    V3 camTarget_ = { 0.0f, 0.0f, 0.0f };         // controls.target (0,0,0)
    float orbitThetaDelta_ = 0.0f;                // 阻尼增量 θ (rad)
    float orbitPhiDelta_ = 0.0f;                  // 阻尼增量 φ (rad)
    bool hover_ = false;                          // 悬停态 (Viewport 侧)
    bool morphActive_ = false;                    // 形变开关 (粒子侧, 与 hover_ 同步)
    bool clickActive_ = false;                    // 点击态 (两侧共用, 始终同步)
    struct Flight {                               // §2.1 一次相机飞行
        bool active = false;
        V3 fromPos{ 0.0f, 0.0f, 0.0f };
        V3 toPos{ 0.0f, 0.0f, 0.0f };
        V3 fromTarget{ 0.0f, 0.0f, 0.0f };
        V3 toTarget{ 0.0f, 0.0f, 0.0f };
        float elapsed = 0.0f;
        float duration = 0.0f;
    } flight_;
    V3 savedPos_{ 0.0f, 0.0f, 0.0f };             // 悬停前位姿 (§1.1 初值 0,0,0)
    V3 savedTarget_{ 0.0f, 0.0f, 0.0f };
    V3 savedClickPos_{ 0.0f, 0.0f, 0.0f };        // 点击前位姿 (§1.1 初值 0,0,0)
    V3 savedClickTarget_{ 0.0f, 0.0f, 0.0f };
    bool pointerDown_ = false;                    // pointerDownAt 是否有效 (§1.1)
    float downX_ = 0.0f, downY_ = 0.0f;           // 按下落点 (CLICK_SLOP 判定)
    float lastX_ = 0.0f, lastY_ = 0.0f;           // 上次指针位 (拖拽增量)

    // ---- 形变状态 (SphereParticles 侧, §3) ----
    float morphClock_ = 0.0f;                     // B19 ∈ [0, 1.1]
    float clickClock_ = 0.0f;                     // B20 ∈ [0, 1.0]
    std::vector<V3> disp_;                        // 显示坐标 (blendMorph 输出)
    std::vector<V3> morphTarget_;                 // 汇聚目标 (§3.5)
    std::vector<V3> clickTarget_;                 // 点击态目标 (§3.6)
    std::vector<float> morphDelay_;               // 每颗入场延迟 [0, 0.35]
    std::vector<float> shapePath_, shapePathSpeed_, shapePathAim_;
    std::vector<float> shapeTube_, shapeTubeSpeed_, shapeTubeAim_;
    std::vector<float> shapeHold_;                // 形态换向倒计时 [3, 8]
    std::vector<float> middleZ_;                  // 中间层固定高度
    std::vector<float> ballPath_, ballTube_;      // 球面点 φ / θ
    std::vector<unsigned char> shapeKind_;        // 1 甜甜圈 / 2 六边形管 / 3 中间层
    std::vector<signed char> ringOf_;             // -1 球面点, 0..3 某只环
    std::vector<float> focus_;                    // §3.8 失焦虚化 [0,1]
    float hexAngle_ = 0.0f;                       // 六边形整体自转累积角
    float torusAngle_ = 0.0f;                     // 甜甜圈翻滚累积角
    Quat gimbalOrientation_{ 0.0f, 0.0f, 0.0f, 1.0f };   // 装配整体姿态 (§3.6a)
    Quat gimbalFixedLocal_[4]{ {0.0f,0.0f,0.0f,1.0f}, {0.0f,0.0f,0.0f,1.0f},
                               {0.0f,0.0f,0.0f,1.0f}, {0.0f,0.0f,0.0f,1.0f} };
    Quat ringQuaternion_[4]{ {0.0f,0.0f,0.0f,1.0f}, {0.0f,0.0f,0.0f,1.0f},
                             {0.0f,0.0f,0.0f,1.0f}, {0.0f,0.0f,0.0f,1.0f} };

    // ---- N2 方法 ----
    void InitMorphState();                       // §3.3 初值 + 环分配 + 首帧显示坐标
    void InitBallFromSites(int hexCount);        // §5.3 点阵开局均摊 (表唯一消费点)
    void ParticleUpdate(double dt);              // §5.4 时钟 → simulate → 门控 → blend → sync
    void UpdateShapeMotion(float dt, bool swap); // §3.2
    void UpdateTargets(float dt, bool swap);     // §3.4~§3.6
    void BlendMorph();                           // §3.7 显示坐标
    void SyncBuffers();                          // §3.8 focus
    void UpdateCamera(double dt);                // §1.4 B15~B18
    void ControlsUpdate();                       // §3.2 阻尼旋转
    bool ControlsEnabled() const;                // B15
    void ViewBasis(V3& fwd, V3& right, V3& up) const;   // 相机视图基 (含退化保护)
    bool HitsBall(float x, float y) const;       // §8.2 射线 vs r=2 球壳
    void EnterHover();                           // §1.3 B13
    void ExitHover();
    void EnterClick();                           // §1.3 B10/B11/B12
    void ReverseClick();                         // §1.3 B14
    void StartFlight(V3 toPos, V3 toTarget, float duration);
    float DipW() const { return dpiScale_ > 0.0f ? pxW_ / dpiScale_ : 0.0f; }
    float DipH() const { return dpiScale_ > 0.0f ? pxH_ / dpiScale_ : 0.0f; }
};
