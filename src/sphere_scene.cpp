// ============================================================
// sphere_scene.cpp — N1 + N2 球体场景原生实现 (task-8 / N2 见 docs/scene-n2-spec.md)
// N1 数值对照 native-shell/docs/sphere-port-spec.md (430 行 6 章), 结构见 sphere_scene.h。
// ============================================================
#include "sphere_scene.h"

#include <d3d11.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <algorithm>
#include <cmath>

// D2D / D3D11 系统库 (零第三方; CMake 侧再显式链接一次, 双保险)
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")

using namespace winrt;
using namespace Windows::UI::Composition;
using winrt::Windows::Graphics::DirectX::DirectXAlphaMode;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

namespace {

// ---- 规格 §6.1 N1 常量 (全部摘自 sphere-port-spec.md) ----
constexpr float kMinSpeed = 0.12f;      // 漂移线速度下限 mm/s
constexpr float kMaxSpeed = 0.42f;      // 上限
constexpr float kMinHold = 3.0f;        // 换向间隔下限 s
constexpr float kMaxHold = 8.0f;        // 上限
constexpr float kAimTau = 0.8f;         // AIM_TAU 转向指数平滑时间常数 s
constexpr float kMaxTurnRad = 110.0f * 3.14159265358979f / 180.0f;  // MAX_TURN_DEG = ±110°
constexpr float kMinPulse = 0.2f;       // 速度正弦调制角频率下限 rad/s
constexpr float kMaxPulse = 0.7f;       // 上限
constexpr float kSpeedMod = 0.2f;       // 调制幅度 ±20%
constexpr float kBackDim = 0.5f;        // BACK_DIM 背向端点保底亮度
constexpr float kFadeDrop = 0.75f;      // FADE_DROP 最远端保底 0.25
constexpr float kLineOpacity = 0.9f;    // LINE_OPACITY
constexpr float kNear = 0.05f;          // 相机 near (视深 <= near 的点跳过)
constexpr float kCamX = 9.5f;           // §3.1 初始相机位
constexpr float kCamY = 4.8f;
constexpr float kCamZ = 11.5f;
constexpr float kTwoPi = 6.28318530717959f;

// §1.1 三层: size (世界单位) / opacity
constexpr float kLayerSize[3] = { 0.14f, 0.22f, 0.32f };
constexpr float kLayerOpacity[3] = { 0.5f, 0.8f, 1.0f };

// ---- 规格 docs/scene-n2-spec.md §0 N2 常量 ----
constexpr float kPi = 3.14159265358979f;
constexpr float kHoverFlyTime = 0.9f;     // HOVER_FLY_TIME (Viewport.ts:9)
constexpr float kClickFlyTime = 1.2f;     // CLICK_FLY_TIME (:21)
constexpr float kHoverMinDist = 8.0f;     // HOVER_MIN_DISTANCE (:17)
constexpr float kHoverMaxDist = 14.0f;    // HOVER_MAX_DISTANCE (:18)
constexpr float kClickSlop = 5.0f;        // CLICK_SLOP px (:24)
constexpr float kRotateSpeed = 0.65f;     // controls.rotateSpeed
constexpr float kDamping = 0.08f;         // controls.dampingFactor
constexpr float kDampDecay = 0.92f;       // 1 − dampingFactor 阻尼衰减
constexpr float kMaxPolar = 1.570796326794897f - 0.02f;   // maxPolarAngle = π/2 − 0.02
constexpr float kMinOrbitDist = 1.5f;     // minDistance
constexpr float kMaxOrbitDist = 120.0f;   // maxDistance (enableZoom=false 故无滚轮缩放)
constexpr float kMorphDuration = 0.75f;   // MORPH_DURATION
constexpr float kMorphStagger = 0.35f;    // MORPH_STAGGER
constexpr float kMorphTotal = 1.1f;       // MORPH_TOTAL = 0.75 + 0.35
constexpr float kClickSwapDur = 1.0f;     // CLICK_SWAP_DURATION
constexpr float kTorusMajorR = 0.42f, kTorusMinorR = 0.12f;
constexpr float kHexTubeR = 0.08f, kHexMiterR = 0.04f;
constexpr float kHexSpinRad = -0.10471975511965978f;      // HEX_SPIN_DEG_PER_SEC = −6°/s
constexpr float kTorusTumbleRad = 0.24434609527920613f;   // TORUS_TUMBLE = +14°/s
constexpr float kTorusAxisTilt = 0.5f, kTorusDriftX = 0.11f, kTorusDriftY = 0.07f;
constexpr float kShapeTubeSpeed = 0.8f;   // SHAPE_TUBE_SPEED
constexpr float kTorusPathSpeed = 0.45f;  // TORUS_PATH_SPEED
constexpr float kHexPathSpeed = 0.4f;     // HEX_PATH_SPEED
constexpr float kMiddleInnerR = 0.575f, kMiddleOuterR = 0.75f, kMiddleHeightR = 0.125f;
constexpr float kMiddlePathSpeed = 0.3f;  // MIDDLE_PATH_SPEED
constexpr float kRingMajorR = 0.88f, kRingMinorR = 0.1015f;
constexpr float kAssemblySpinRad = 0.41887902047863906f;  // ASSEMBLY_SPIN = 24°/s
constexpr float kAssemblyDriftA = 0.09f, kAssemblyDriftB = 0.13f, kAssemblyDriftC = 0.07f;
constexpr float kAssemblySpinWave = 0.5f; // ASSEMBLY_SPIN_WAVE
constexpr float kBallRadiusR = 0.25f;     // BALL_RADIUS_RATIO (Ø1mm 球面点)
constexpr float kBallPathSpeed = 0.45f, kBallTubeSpeed = 0.35f;
constexpr float kFocusXRamp = 0.15f;      // FOCUS_X_RAMP
constexpr float kFocusGrow = 2.0f;        // FOCUS_BLUR_GROW
constexpr float kFocusFade = 0.85f;       // FOCUS_BLUR_FADE
constexpr int kRingCount = 4;             // RING_COUNT (正四面体 4 环 × 120 颗)

} // namespace

// ---------------- 随机 (规格 §1.3) ----------------

float SphereScene::Rand() {
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng_);
}

float SphereScene::RandBetween(float a, float b) {
    return a + (b - a) * Rand();
}

float SphereScene::Gauss() {
    float u = 0.0f;
    do { u = Rand(); } while (u <= 0.0f);   // rand(0,1) 排除 0
    float v = Rand();
    return std::sqrt(-2.0f * std::log(u)) * std::cos(kTwoPi * v);  // Box-Muller
}

SphereScene::V3 SphereScene::RandomUnitVector() {
    for (int t = 0; t < 8; ++t) {           // 最多重试 8 次
        V3 g{ Gauss(), Gauss(), Gauss() };
        float len = Length(g);
        if (len > 1e-4f) return g * (1.0f / len);
    }
    return V3{ 1.0f, 0.0f, 0.0f };          // 退化兜底
}

SphereScene::V3 SphereScene::RandomTangent(V3 p) {
    for (int t = 0; t < 8; ++t) {
        V3 g = RandomUnitVector();
        // 正确切平面投影 (规格注: 源码未除 r², simulate 每帧重投影, 视觉无差别)
        V3 q = g - p * (Dot(g, p) / Dot(p, p));
        float len = Length(q);
        if (len > 1e-3f) return q * (1.0f / len);
    }
    return Normalize(V3{ -p.y, p.x, 0.0f }); // 退化兜底
}

// ---------------- 向量工具 ----------------

float SphereScene::Dot(V3 a, V3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

SphereScene::V3 SphereScene::Cross(V3 a, V3 b) {
    return V3{ a.y * b.z - a.z * b.y,
               a.z * b.x - a.x * b.z,
               a.x * b.y - a.y * b.x };
}

float SphereScene::Length(V3 a) {
    return std::sqrt(Dot(a, a));
}

SphereScene::V3 SphereScene::Normalize(V3 a) {
    float len = Length(a);
    if (len <= 1e-12f) return V3{ 0.0f, 0.0f, 0.0f };
    return a * (1.0f / len);
}

SphereScene::V3 SphereScene::RotateAxis(V3 v, V3 axis, float angle) {
    float c = std::cos(angle);
    float s = std::sin(angle);
    // Rodrigues 旋转公式
    return v * c + Cross(axis, v) * s + axis * (Dot(axis, v) * (1.0f - c));
}

// ---------------- 初始化 (规格 §1.3 / §6.1) ----------------

void SphereScene::InitParticles() {
    particles_.assign(kParticles, Particle{});
    for (int i = 0; i < kParticles; ++i) {
        Particle& p = particles_[i];
        p.pos = RandomUnitVector() * kRadius;   // 均匀落在半径 2 球面
        p.aim = RandomTangent(p.pos);           // 目标朝向 = 初值朝向 (开局不集体转向)
        p.dir = p.aim;
        p.speed = RandBetween(kMinSpeed, kMaxSpeed);  // 0.12 ~ 0.42 mm/s
        p.hold = RandBetween(kMinHold, kMaxHold);     // 3 ~ 8 s 换向
        p.phase = RandBetween(0.0f, kTwoPi);
        p.pulse = RandBetween(kMinPulse, kMaxPulse);  // 0.2 ~ 0.7 rad/s
    }
}

bool SphereScene::Init(Compositor const& compositor, float w, float h) {
    if (initialized_) return true;
    compositor_ = compositor;

    // 1) D3D11 设备 (硬件优先, WARP 兜底) 提供 IDXGIDevice
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;   // D2D 要求 BGRA 支持
    D3D_FEATURE_LEVEL levels[3] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    winrt::com_ptr<ID3D11Device> d3d;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, static_cast<UINT>(3), D3D11_SDK_VERSION, d3d.put(), &got, nullptr);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            levels, static_cast<UINT>(3), D3D11_SDK_VERSION, d3d.put(), &got, nullptr);
    }
    if (FAILED(hr) || !d3d) return false;

    // 2) D2D 设备
    winrt::com_ptr<IDXGIDevice> dxgi;
    hr = d3d->QueryInterface(IID_PPV_ARGS(dxgi.put()));
    if (FAILED(hr)) return false;
    hr = D2D1CreateDevice(dxgi.get(), nullptr, d2dDevice_.put());
    if (FAILED(hr) || !d2dDevice_) return false;

    // 3) Composition 图形设备 (ICompositorInterop::CreateGraphicsDevice)
    auto compInterop = compositor_.as<ABI::Windows::UI::Composition::ICompositorInterop>();
    ABI::Windows::UI::Composition::ICompositionGraphicsDevice* rawGd = nullptr;
    hr = compInterop->CreateGraphicsDevice(d2dDevice_.get(), &rawGd);
    if (FAILED(hr) || !rawGd) return false;
    hr = rawGd->QueryInterface(guid_of<CompositionGraphicsDevice>(),
        reinterpret_cast<void**>(put_abi(graphicsDevice_)));
    rawGd->Release();
    if (FAILED(hr) || !graphicsDevice_) return false;

    // 4) 视觉体 (不透明黑底表面; 尺寸由 Resize 决定)
    visual_ = compositor_.CreateSpriteVisual();

    // 5) 粒子与连线状态 (规格 §1.3 / §2.1)
    InitParticles();
    facing_.assign(kParticles, 0.0f);
    nbrIdx_.assign(kParticles, std::array<int, kNearest>{});
    nbrD2_.assign(kParticles, std::array<float, kNearest>{});
    kn_.assign(kParticles, 0);
    farthest_.assign(kParticles, 0.0f);
    edges_.assign(kMaxEdges, Edge{});
    sx_.assign(kParticles, 0.0f);
    sy_.assign(kParticles, 0.0f);
    depth_.assign(kParticles, 0.0f);
    psize_.assign(kParticles, 0.0f);

    // §1.2 / §3.1: f = 1/tan(fov/2), fov = 50° → f = 1/tan25° ≈ 2.1445
    f_ = 1.0f / std::tan(50.0f * 3.14159265358979f / 180.0f * 0.5f);

    // N2 相机状态化 (Viewport.ts:99-113): 初始位 (9.5,4.8,11.5) → target (0,0,0)
    camPos_ = V3{ kCamX, kCamY, kCamZ };
    camTarget_ = V3{ 0.0f, 0.0f, 0.0f };

    // N2 形变状态 (§3.3): 形态参数 / 环分配 / 点阵开局均摊 / 首帧显示坐标
    InitMorphState();

    initialized_ = true;
    if (w > 0.0f && h > 0.0f) Resize(w, h);   // 传 0 则等 Layout 调 Resize
    return true;
}

// ---------------- 尺寸同步 (规格 §5.5) ----------------

void SphereScene::SetDpiScale(float scale) {
    if (scale > 0.0f) dpiScale_ = scale;      // min(devicePixelRatio, 2) 的底数
}

void SphereScene::Resize(float w, float h) {
    if (visual_) visual_.Size({ w, h });
    if (!graphicsDevice_) return;             // Init 之前 Layout 已调到: 跳过
    int pxW = static_cast<int>(w * dpiScale_ + 0.5f);
    int pxH = static_cast<int>(h * dpiScale_ + 0.5f);
    if (pxW < 0) pxW = 0;
    if (pxH < 0) pxH = 0;
    if (pxW == 0 || pxH == 0) {               // 最小化: 暂停绘制, 恢复时按新尺寸重建
        pxW_ = 0;
        pxH_ = 0;
        return;
    }
    if (surface_ && pxW == pxW_ && pxH == pxH_) return;   // 尺寸没变
    CreateSurface(pxW, pxH);
}

bool SphereScene::CreateSurface(int pxW, int pxH) {
    try {
        // 表面尺寸 = 像素尺寸 (投影按设备像素算, 见 §1.2 点大小公式)
        auto newSurface = graphicsDevice_.CreateDrawingSurface(
            Windows::Foundation::Size{ static_cast<float>(pxW), static_cast<float>(pxH) },
            DirectXPixelFormat::B8G8R8A8UIntNormalized,
            DirectXAlphaMode::Premultiplied);
        if (!brush_) {
            brush_ = compositor_.CreateSurfaceBrush();
            brush_.Stretch(CompositionStretch::Fill);   // 表面按比例铺满视觉体
            visual_.Brush(brush_);
        }
        brush_.Surface(newSurface);
        surface_ = newSurface;
        pxW_ = pxW;
        pxH_ = pxH;
        aspect_ = static_cast<float>(pxW) / static_cast<float>(pxH);
        return true;
    } catch (hresult_error const&) {
        pxW_ = 0;
        pxH_ = 0;
        return false;
    }
}

// ---------------- 每帧漂移 (规格 §1.4) ----------------

void SphereScene::Simulate(double dt) {
    float fd = static_cast<float>(dt);
    elapsed_ += fd;
    float decay = 1.0f - std::exp(-fd / kAimTau);   // AIM_TAU = 0.8 s 指数平滑

    for (int i = 0; i < kParticles; ++i) {
        Particle& p = particles_[i];

        // (a) 到期随机改向: 3~8 s 换向, 单次最多 ±110°
        p.hold -= fd;
        if (p.hold <= 0.0f) {
            p.hold = RandBetween(kMinHold, kMaxHold);
            V3 n = Normalize(p.pos);                 // 球面法线
            V3 c = Cross(n, p.aim);                  // 转轴 (切平面内)
            if (Length(c) > 1e-4f) {
                p.aim = RotateAxis(p.aim, Normalize(c), RandBetween(-kMaxTurnRad, kMaxTurnRad));
            }
        }

        // (b) 朝向指数平滑 + 投回切平面 (r²=4) + 归一化
        V3 dir = p.dir + (p.aim - p.dir) * decay;
        dir = dir - p.pos * (Dot(dir, p.pos) / 4.0f);
        if (Length(dir) < 1e-6f) dir = RandomTangent(p.pos);
        p.dir = Normalize(dir);

        // (c) 变速步进 (0.8~1.2 倍正弦起伏) + 归一回半径 2 球面
        float t = static_cast<float>(elapsed_);   // 避免 double→float 警告 (W3)
        float v = p.speed * (1.0f + kSpeedMod * std::sin(t * p.pulse + p.phase));
        V3 np = p.pos + p.dir * (v * fd);
        p.pos = np * (kRadius / Length(np));
    }
}

// ---------------- 近邻与连线 (规格 §2) ----------------

void SphereScene::InsertNeighbor(int i, int j, float distSq) {
    int cnt = kn_[i];
    if (cnt == kNearest) {
        if (distSq >= nbrD2_[i][kNearest - 1]) return;   // >= 第12名 → 直接跳过
        int pos = kNearest - 1;
        while (pos > 0 && nbrD2_[i][pos - 1] > distSq) { // 升序插入排序
            nbrD2_[i][pos] = nbrD2_[i][pos - 1];
            nbrIdx_[i][pos] = nbrIdx_[i][pos - 1];
            --pos;
        }
        nbrD2_[i][pos] = distSq;
        nbrIdx_[i][pos] = j;
    } else {
        int pos = cnt;
        while (pos > 0 && nbrD2_[i][pos - 1] > distSq) {
            nbrD2_[i][pos] = nbrD2_[i][pos - 1];
            nbrIdx_[i][pos] = nbrIdx_[i][pos - 1];
            --pos;
        }
        nbrD2_[i][pos] = distSq;
        nbrIdx_[i][pos] = j;
        kn_[i] = cnt + 1;
    }
}

bool SphereScene::HasNeighbor(int i, int j) const {
    for (int k = 0; k < kn_[i]; ++k) {
        if (nbrIdx_[i][k] == j) return true;
    }
    return false;
}

void SphereScene::UpdateLinks() {
    // §2.3 步骤1: facing = 0.5 + 0.5 * max(dot(normalize(pos), normalize(cam-pos)), 0)
    // N2: 连线读显示坐标 (blendMorph 输出) 与当前成员相机 —— spec §2.3 / §3.7
    const V3& camPos = camPos_;
    for (int i = 0; i < kParticles; ++i) {
        V3 n = Normalize(disp_[i]);
        V3 toCam = Normalize(camPos - disp_[i]);
        float d = Dot(n, toCam);
        facing_[i] = kBackDim + (1.0f - kBackDim) * (d > 0.0f ? d : 0.0f);
    }

    // §2.1: 每帧暴力 O(N²) 求每颗 12 近邻 (平方距离升序, N=768 → 29.5 万对, 毫秒级)
    std::fill(kn_.begin(), kn_.end(), 0);
    for (int i = 0; i < kParticles; ++i) {
        for (int j = i + 1; j < kParticles; ++j) {
            float dx = disp_[i].x - disp_[j].x;
            float dy = disp_[i].y - disp_[j].y;
            float dz = disp_[i].z - disp_[j].z;
            float d2 = dx * dx + dy * dy + dz * dz;
            InsertNeighbor(i, j, d2);
            InsertNeighbor(j, i, d2);
        }
    }
    for (int i = 0; i < kParticles; ++i) {
        farthest_[i] = std::sqrt(nbrD2_[i][kNearest - 1]);   // 第12名平方距离开方
    }

    // §2.2/§2.3: 发边 (i,j); i>j 且 j 侧含 i 则跳过 (该边由 j 侧发出); 容量硬顶 8192
    edgeCount_ = 0;
    for (int i = 0; i < kParticles && edgeCount_ < kMaxEdges; ++i) {
        for (int k = 0; k < kNearest && edgeCount_ < kMaxEdges; ++k) {
            int j = nbrIdx_[i][k];
            if (i > j && HasNeighbor(j, i)) continue;
            float d = std::sqrt(nbrD2_[i][k]);
            float ref = std::max(std::max(farthest_[i], farthest_[j]), 1e-6f);
            edges_[edgeCount_].i = i;
            edges_[edgeCount_].j = j;
            edges_[edgeCount_].fade = 1.0f - kFadeDrop * (d / ref);   // FADE_DROP=0.75
            ++edgeCount_;
        }
    }
}

// ---------------- 投影与绘制 (规格 §1.2 / §2.4 / §5.5) ----------------

void SphereScene::Draw() {
    if (!surface_ || pxW_ <= 0 || pxH_ <= 0) return;

    auto interop = surface_.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
    RECT update{ 0, 0, pxW_, pxH_ };
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT offset{};
    HRESULT hr = interop->BeginDraw(&update, IID_PPV_ARGS(dc.put()), &offset);
    if (FAILED(hr) || !dc) return;

    // 若上下文 DPI != 96, 用变换把用户坐标校正到表面像素 (1px 线宽同样被校正)
    float dpiX = 96.0f, dpiY = 96.0f;
    dc->GetDpi(&dpiX, &dpiY);
    if (dpiX <= 0.0f) dpiX = 96.0f;
    if (dpiY <= 0.0f) dpiY = 96.0f;
    D2D1_MATRIX_3X2_F xf = D2D1::Matrix3x2F::Scale(96.0f / dpiX, 96.0f / dpiY);
    if (offset.x != 0 || offset.y != 0) {
        xf = xf * D2D1::Matrix3x2F::Translation(
            static_cast<float>(-offset.x) * 96.0f / dpiX,
            static_cast<float>(-offset.y) * 96.0f / dpiY);
    }
    dc->SetTransform(&xf);

    // §5.5: clearColor 0x000000 alpha 1
    D2D1_COLOR_F black{ 0.0f, 0.0f, 0.0f, 1.0f };
    dc->Clear(&black);
    // §1.1/§2.4: AdditiveBlending (dst += src.rgb * src.a), 对黑底即为纯叠加
    dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);

    // §3.1/N2: 成员相机 (初始 (9.5, 4.8, 11.5) → camTarget_; 飞行/拖拽后即新位姿)
    const V3 camPos = camPos_;
    V3 fwd, right, up;
    ViewBasis(fwd, right, up);

    // §1.2 投影 + 点大小 (照抄 three.js, 不含 tan(fov/2)):
    //   pointSizePx = size * (heightDevicePx * 0.5) / (-p_v.z)
    float halfH = static_cast<float>(pxH_) * 0.5f;
    const float scale = sphereScale_;            // 设置: 球形大小 (世界坐标统一缩放)
    for (int i = 0; i < kParticles; ++i) {
        V3 rel = disp_[i] * scale - camPos;      // N2: 显示坐标×缩放 (连线端点读 sx_/sy_,
                                                  //     走同一投影路径自动一致, §3.7)
        float depth = Dot(rel, fwd);            // clip.w = -p_v.z = 视深
        depth_[i] = depth;
        if (depth <= kNear) {
            sx_[i] = -1.0e6f;
            sy_[i] = -1.0e6f;
            psize_[i] = 0.0f;
            continue;
        }
        float ndcX = (f_ / aspect_) * Dot(rel, right) / depth;
        float ndcY = f_ * Dot(rel, up) / depth;
        sx_[i] = (ndcX + 1.0f) * 0.5f * static_cast<float>(pxW_);
        sy_[i] = (1.0f - ndcY) * 0.5f * static_cast<float>(pxH_);
        psize_[i] = kLayerSize[i / kPerLayer] * halfH / depth;
        psize_[i] *= 1.0f + kFocusGrow * focus_[i];   // §4.2 点大小 ×(1 + 2·focus)
    }

    // §2.4 连线: 灰度亮度 × LINE_OPACITY 0.9, 固定 1px, 加色; renderOrder 3 → 先画
    winrt::com_ptr<ID2D1SolidColorBrush> lineBrush;
    D2D1_COLOR_F white{ 1.0f, 1.0f, 1.0f, 1.0f };
    dc->CreateSolidColorBrush(&white, nullptr, lineBrush.put());
    if (lineBrush) {
        for (int e = 0; e < edgeCount_; ++e) {
            int i = edges_[e].i;
            int j = edges_[e].j;
            if (depth_[i] <= kNear || depth_[j] <= kNear) continue;
            float fade = edges_[e].fade;
            float b0 = fade * facing_[i] * kLineOpacity;   // 端点亮度 = fade × facing × 0.9
            float b1 = fade * facing_[j] * kLineOpacity;
            // three.js 顶点色沿线线性插值 → 2 段常色阶梯近似 (各取段中点色)
            float seg0 = b0 * 0.75f + b1 * 0.25f;
            float seg1 = b0 * 0.25f + b1 * 0.75f;
            D2D1_POINT_2F p0{ sx_[i], sy_[i] };
            D2D1_POINT_2F p1{ sx_[j], sy_[j] };
            D2D1_POINT_2F mid{ (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f };
            D2D1_COLOR_F c0{ seg0, seg0, seg0, 1.0f };
            lineBrush->SetColor(&c0);
            dc->DrawLine(p0, mid, lineBrush.get(), 1.0f);
            D2D1_COLOR_F c1{ seg1, seg1, seg1, 1.0f };
            lineBrush->SetColor(&c1);
            dc->DrawLine(mid, p1, lineBrush.get(), 1.0f);
        }
    }

    // §1.1 粒子: 64×64 径向渐变点贴图四段色标 × 层 opacity; renderOrder 4 → 后画
    D2D1_GRADIENT_STOP stops[4] = {
        { 0.00f, D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 1.00f } },   // t=0    → alpha 1
        { 0.35f, D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.90f } },   // t=0.35 → 0.9
        { 0.70f, D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.28f } },   // t=0.7  → 0.28
        { 1.00f, D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.00f } },   // t=1    → 0
    };
    winrt::com_ptr<ID2D1GradientStopCollection> stopCollection;
    dc->CreateGradientStopCollection(stops, 4, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
        stopCollection.put());
    if (stopCollection) {
        for (int layer = 0; layer < 3; ++layer) {
            D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES rp{};
            rp.center = D2D1_POINT_2F{ 0.0f, 0.0f };
            rp.gradientOriginOffset = D2D1_POINT_2F{ 0.0f, 0.0f };
            rp.radiusX = 1.0f;
            rp.radiusY = 1.0f;
            D2D1_BRUSH_PROPERTIES bp{};
            bp.opacity = kLayerOpacity[layer];    // finalAlpha = layerOpacity × texAlpha
            bp.transform = D2D1::Matrix3x2F::Identity();
            winrt::com_ptr<ID2D1RadialGradientBrush> rb;
            dc->CreateRadialGradientBrush(&rp, &bp, stopCollection.get(), rb.put());
            if (!rb) continue;
            int begin = layer * kPerLayer;
            int end = begin + kPerLayer;
            for (int i = begin; i < end; ++i) {
                float r = psize_[i] * 0.5f;       // gl_PointSize 是全宽, 半径 = 全宽/2
                if (depth_[i] <= kNear || r <= 0.0f) continue;
                // §4.2 失焦: alpha ×(1 − 0.85·focus); focus=0 时与层 opacity 相同 (N1 恒等)
                rb->SetOpacity(kLayerOpacity[layer] * (1.0f - kFocusFade * focus_[i]));
                D2D1_POINT_2F c{ sx_[i], sy_[i] };
                rb->SetCenter(c);
                rb->SetRadiusX(r);
                rb->SetRadiusY(r);
                D2D1_ELLIPSE el{ c, r, r };
                dc->FillEllipse(&el, rb.get());
            }
        }
    }

    // 设置: 鼠标拖尾 (加色混合渐隐, 画在 overlayDraw_ 之前)
    DrawTrail(dc.get());

    // Stage3 覆盖层: 场景之后绘制; 混合切回 SOURCE_OVER (场景用 ADD)
    if (overlayDraw_) {
        ID2D1Bitmap1* snap = nullptr;
        if (snapshotEnabled_ && EnsureSnapshot(dc.get())) {
            // D2D 批量命令先落地, 否则源位图未提交 -> 拷贝失败读到显存垃圾 (26MB 噪声 PNG 实证)
            dc->Flush();
            HRESULT hrCopy = snapshotBmp_->CopyFromRenderTarget(nullptr, dc.get(), nullptr);
            if (SUCCEEDED(hrCopy)) {
                snap = snapshotBmp_.get();
            } else {
                static int copyFailLog = 0;
                if (++copyFailLog <= 3) {
                    char b[96];
                    sprintf_s(b, "[wp] snapshot copy FAILED hr=0x%08X\n", (unsigned)hrCopy);
                    OutputDebugStringA(b);
                }
                snap = nullptr;   // UI 走纯色兜底
            }
        }
        dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        overlayDraw_(dc.get(), snap, pxW_ / dpiScale_, pxH_ / dpiScale_);
    }

    if (SUCCEEDED(interop->EndDraw())) ++frames_;
}

// ---------------- 帧入口 (规格 §5.1) ----------------

void SphereScene::Tick(double dt) {
    if (!initialized_) return;
    ++ticks_;
    if (animationsEnabled_) {                    // 设置: 动效总开关关 -> 时钟冻结
        UpdateCamera(dt);                        // §5.2 B15~B18 (dt 已由 main 钳到 ≤0.1)
        ParticleUpdate(dt);                      // §5.4 (含 Simulate; dt<=0 内部直接返回)
    }
    if (pxW_ <= 0 || pxH_ <= 0 || !surface_) return;   // 最小化: 模拟继续, 跳过绘制
    UpdateLinks();
    Draw();
}

// ==================== N2 交互态 (docs/scene-n2-spec.md) ====================

namespace {

// easing.ts:2-6 三次缓入缓出 (相机飞行与粒子汇聚共用)
float EaseInOutCubic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (t < 0.5f) return 4.0f * t * t * t;
    float u = -2.0f * t + 2.0f;
    return 1.0f - u * u * u * 0.5f;
}

// 把越界的周期量收回 [0, period)
float WrapPeriod(float value, float period) {
    float m = std::fmod(value, period);
    if (m < 0.0f) m += period;
    return m;
}

float Clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ---- hexSphereSites.ts:17-153 —— 1215 个 float (405 站 × xyz, 4 位小数) ----
// 全部离线烘焙 (纬度环密铺 + 球面弹簧松弛 8000 轮); 运行时零生成,
// 唯一消费点 InitBallFromSites (SphereParticles.ts:566-585)。
constexpr float kHexSites[1215] = {
  0.1513f, 0.0829f, 0.9850f, -0.0350f, 0.0937f, 0.9950f, 0.0483f, -0.0730f, 0.9962f,
  0.2516f, 0.2338f, 0.9392f, 0.0676f, 0.2474f, 0.9666f, -0.1170f, 0.2591f, 0.9587f,
  -0.2185f, 0.1008f, 0.9706f, -0.1363f, -0.0653f, 0.9885f, -0.0621f, -0.2349f, 0.9700f,
  0.1262f, -0.2368f, 0.9633f, 0.2330f, -0.0826f, 0.9690f, 0.3328f, 0.0728f, 0.9402f,
  0.4228f, 0.2513f, 0.8707f, 0.1698f, 0.3897f, 0.9051f, -0.0125f, 0.4076f, 0.9131f,
  -0.1942f, 0.4169f, 0.8880f, -0.2964f, 0.2625f, 0.9183f, -0.3939f, 0.1010f, 0.9136f,
  -0.3162f, -0.0650f, 0.9464f, -0.2478f, -0.2380f, 0.9391f, -0.1564f, -0.3964f, 0.9046f,
  0.0268f, -0.3904f, 0.9203f, 0.2070f, -0.3887f, 0.8978f, 0.3099f, -0.2427f, 0.9193f,
  0.5008f, 0.0808f, 0.8618f, 0.5817f, 0.2441f, 0.7759f, 0.3389f, 0.4166f, 0.8436f,
  0.0980f, 0.5389f, 0.8366f, -0.0842f, 0.5565f, 0.8266f, -0.2638f, 0.5596f, 0.7857f,
  -0.3696f, 0.4130f, 0.8324f, -0.4647f, 0.2586f, 0.8469f, -0.5563f, 0.1082f, 0.8239f,
  -0.4857f, -0.0572f, 0.8723f, -0.4244f, -0.2313f, 0.8754f, -0.3346f, -0.3916f, 0.8572f,
  -0.2321f, -0.5404f, 0.8088f, -0.0528f, -0.5381f, 0.8413f, 0.1276f, -0.5349f, 0.8352f,
  0.3744f, -0.4043f, 0.8345f, 0.4775f, -0.2501f, 0.8423f, 0.4104f, -0.0886f, 0.9076f,
  0.5697f, -0.0881f, 0.8171f, 0.6520f, 0.0750f, 0.7545f, 0.5031f, 0.4103f, 0.7606f,
  0.3919f, 0.5564f, 0.7327f, 0.2247f, 0.6318f, 0.7419f, 0.0456f, 0.6801f, 0.7317f,
  -0.1381f, 0.6905f, 0.7100f, -0.3156f, 0.6844f, 0.6573f, -0.4371f, 0.5402f, 0.7191f,
  -0.5413f, 0.3877f, 0.7461f, -0.6803f, 0.1824f, 0.7098f, -0.6375f, -0.0404f, 0.7694f,
  -0.5861f, -0.2121f, 0.7820f, -0.5033f, -0.3734f, 0.7792f, -0.4056f, -0.5256f, 0.7479f,
  -0.2959f, -0.6652f, 0.6855f, -0.1171f, -0.6713f, 0.7318f, 0.0680f, -0.6764f, 0.7334f,
  0.2469f, -0.6301f, 0.7362f, 0.4114f, -0.5506f, 0.7264f, 0.5319f, -0.4092f, 0.7414f,
  0.6267f, -0.2513f, 0.7377f, 0.7817f, 0.0529f, 0.6214f, 0.7216f, 0.2195f, 0.6566f,
  0.6513f, 0.3886f, 0.6517f, 0.5487f, 0.5427f, 0.6359f, 0.3489f, 0.7007f, 0.6223f,
  0.1727f, 0.7644f, 0.6211f, -0.0099f, 0.7983f, 0.6022f, -0.1938f, 0.7987f, 0.5697f,
  -0.3690f, 0.7798f, 0.5058f, -0.4825f, 0.6524f, 0.5845f, -0.5954f, 0.5102f, 0.6207f,
  -0.6872f, 0.3496f, 0.6368f, -0.7567f, 0.0247f, 0.6533f, -0.7303f, -0.1594f, 0.6643f,
  -0.6680f, -0.3331f, 0.6654f, -0.5748f, -0.4920f, 0.6539f, -0.4658f, -0.6357f, 0.6156f,
  -0.3532f, -0.7643f, 0.5396f, -0.1762f, -0.7828f, 0.5968f, 0.0100f, -0.7951f, 0.6064f,
  0.1934f, -0.7629f, 0.6169f, 0.3692f, -0.6961f, 0.6158f, 0.5615f, -0.5481f, 0.6199f,
  0.6732f, -0.3992f, 0.6224f, 0.7562f, -0.2368f, 0.6099f, 0.7086f, -0.0905f, 0.6998f,
  0.8780f, -0.0004f, 0.4786f, 0.8492f, 0.1743f, 0.4985f, 0.7811f, 0.3369f, 0.5258f,
  0.7029f, 0.4964f, 0.5094f, 0.5981f, 0.6370f, 0.4863f, 0.4590f, 0.7460f, 0.4825f,
  0.2929f, 0.8216f, 0.4890f, 0.1159f, 0.8709f, 0.4776f, -0.0656f, 0.8894f, 0.4525f,
  -0.2446f, 0.8785f, 0.4104f, -0.5306f, 0.7296f, 0.4314f, -0.6514f, 0.5965f, 0.4689f,
  -0.7497f, 0.4434f, 0.4913f, -0.8105f, 0.2732f, 0.5182f, -0.8494f, 0.0963f, 0.5189f,
  -0.8422f, -0.0872f, 0.5321f, -0.8036f, -0.2665f, 0.5321f, -0.7332f, -0.4345f, 0.5230f,
  -0.6337f, -0.5869f, 0.5040f, -0.5146f, -0.7206f, 0.4647f, -0.2355f, -0.8655f, 0.4421f,
  -0.0551f, -0.8854f, 0.4615f, 0.1270f, -0.8697f, 0.4770f, 0.3045f, -0.8235f, 0.4787f,
  0.4691f, -0.7465f, 0.4718f, 0.6112f, -0.6377f, 0.4687f, 0.7236f, -0.5001f, 0.4756f,
  0.8091f, -0.3454f, 0.4755f, 0.8611f, -0.1741f, 0.4776f, 0.9377f, 0.0932f, 0.3347f,
  0.8961f, 0.2725f, 0.3503f, 0.8175f, 0.4384f, 0.3735f, 0.7206f, 0.5970f, 0.3526f,
  0.5839f, 0.7393f, 0.3353f, 0.4153f, 0.8451f, 0.3366f, 0.2402f, 0.9116f, 0.3337f,
  0.0587f, 0.9469f, 0.3160f, -0.1247f, 0.9492f, 0.2890f, -0.3860f, 0.8737f, 0.2959f,
  -0.5476f, 0.7921f, 0.2696f, -0.6775f, 0.6701f, 0.3032f, -0.7838f, 0.5254f, 0.3312f,
  -0.8609f, 0.3602f, 0.3594f, -0.9104f, 0.1819f, 0.3717f, -0.9231f, -0.0061f, 0.3845f,
  -0.9008f, -0.1919f, 0.3895f, -0.8458f, -0.3705f, 0.3839f, -0.7603f, -0.5348f, 0.3686f,
  -0.6487f, -0.6797f, 0.3424f, -0.5135f, -0.8012f, 0.3072f, -0.3511f, -0.8856f, 0.3041f,
  -0.1345f, -0.9425f, 0.3059f, 0.0538f, -0.9454f, 0.3213f, 0.2390f, -0.9139f, 0.3282f,
  0.4157f, -0.8484f, 0.3278f, 0.5820f, -0.7455f, 0.3248f, 0.7259f, -0.6062f, 0.3251f,
  0.8306f, -0.4502f, 0.3278f, 0.9025f, -0.2760f, 0.3307f, 0.9388f, -0.0927f, 0.3318f,
  0.9840f, 0.0001f, 0.1784f, 0.9658f, 0.1864f, 0.1802f, 0.9112f, 0.3642f, 0.1924f,
  0.8226f, 0.5278f, 0.2114f, 0.6979f, 0.6894f, 0.1941f, 0.5260f, 0.8297f, 0.1867f,
  0.3500f, 0.9189f, 0.1818f, 0.1729f, 0.9706f, 0.1676f, -0.0095f, 0.9892f, 0.1461f,
  -0.1908f, 0.9739f, 0.1228f, -0.3620f, 0.9245f, 0.1197f, -0.5207f, 0.8469f, 0.1080f,
  -0.6620f, 0.7377f, 0.1325f, -0.7814f, 0.6027f, 0.1621f, -0.8751f, 0.4447f, 0.1910f,
  -0.9387f, 0.2711f, 0.2130f, -0.9703f, 0.0813f, 0.2279f, -0.9660f, -0.1077f, 0.2351f,
  -0.9275f, -0.2916f, 0.2339f, -0.8567f, -0.4652f, 0.2231f, -0.7568f, -0.6215f, 0.2025f,
  -0.6326f, -0.7551f, 0.1723f, -0.4875f, -0.8616f, 0.1414f, -0.3264f, -0.9361f, 0.1310f,
  -0.1545f, -0.9795f, 0.1291f, 0.0249f, -0.9895f, 0.1424f, 0.2055f, -0.9669f, 0.1515f,
  0.3800f, -0.9114f, 0.1581f, 0.5419f, -0.8238f, 0.1661f, 0.6866f, -0.7053f, 0.1763f,
  0.8184f, -0.5479f, 0.1734f, 0.9111f, -0.3724f, 0.1768f, 0.9659f, -0.1877f, 0.1782f,
  0.9953f, 0.0950f, 0.0184f, 0.9598f, 0.2799f, 0.0227f, 0.8905f, 0.4536f, 0.0343f,
  0.7901f, 0.6108f, 0.0510f, 0.6320f, 0.7737f, 0.0431f, 0.4518f, 0.8916f, 0.0313f,
  0.2797f, 0.9600f, 0.0129f, 0.0985f, 0.9951f, -0.0058f, -0.0877f, 0.9958f, -0.0261f,
  -0.2773f, 0.9599f, -0.0408f, -0.4508f, 0.8914f, -0.0472f, -0.6109f, 0.7907f, -0.0391f,
  -0.7466f, 0.6651f, -0.0131f, -0.8559f, 0.5168f, 0.0172f, -0.9357f, 0.3502f, 0.0434f,
  -0.9835f, 0.1690f, 0.0651f, -0.9971f, -0.0189f, 0.0743f, -0.9762f, -0.2038f, 0.0749f,
  -0.9221f, -0.3806f, 0.0699f, -0.8361f, -0.5458f, 0.0543f, -0.7227f, -0.6907f, 0.0264f,
  -0.5844f, -0.8114f, -0.0043f, -0.4236f, -0.9055f, -0.0248f, -0.2485f, -0.9681f, -0.0318f,
  -0.0639f, -0.9976f, -0.0276f, 0.1224f, -0.9923f, -0.0161f, 0.3052f, -0.9523f, -0.0070f,
  0.4779f, -0.8784f, 0.0019f, 0.6340f, -0.7732f, 0.0128f, 0.7677f, -0.6404f, 0.0235f,
  0.8858f, -0.4636f, 0.0215f, 0.9598f, -0.2798f, 0.0211f, 0.9954f, -0.0941f, 0.0182f,
  0.9896f, 0.0009f, -0.1436f, 0.9717f, 0.1891f, -0.1414f, 0.9193f, 0.3706f, -0.1326f,
  0.8340f, 0.5390f, -0.1177f, 0.7193f, 0.6877f, -0.0985f, 0.5465f, 0.8300f, -0.1110f,
  0.3809f, 0.9142f, -0.1384f, 0.2039f, 0.9660f, -0.1590f, 0.0193f, 0.9843f, -0.1755f,
  -0.1729f, 0.9664f, -0.1903f, -0.3620f, 0.9103f, -0.2009f, -0.5299f, 0.8237f, -0.2017f,
  -0.6792f, 0.7093f, -0.1887f, -0.8036f, 0.5729f, -0.1612f, -0.9000f, 0.4159f, -0.1304f,
  -0.9643f, 0.2438f, -0.1034f, -0.9939f, 0.0660f, -0.0887f, -0.9897f, -0.1130f, -0.0882f,
  -0.9532f, -0.2881f, -0.0917f, -0.8861f, -0.4534f, -0.0963f, -0.7891f, -0.6025f, -0.1198f,
  -0.6626f, -0.7336f, -0.1509f, -0.5108f, -0.8418f, -0.1744f, -0.3397f, -0.9215f, -0.1885f,
  -0.1570f, -0.9692f, -0.1896f, 0.0315f, -0.9828f, -0.1820f, 0.2184f, -0.9606f, -0.1718f,
  0.3969f, -0.9034f, -0.1625f, 0.5612f, -0.8135f, -0.1523f, 0.7052f, -0.6951f, -0.1398f,
  0.8256f, -0.5497f, -0.1274f, 0.9203f, -0.3676f, -0.1336f, 0.9721f, -0.1871f, -0.1413f,
  0.9489f, 0.0933f, -0.3015f, 0.9141f, 0.2768f, -0.2962f, 0.8473f, 0.4496f, -0.2826f,
  0.7488f, 0.6074f, -0.2653f, 0.6261f, 0.7397f, -0.2467f, 0.4712f, 0.8362f, -0.2807f,
  0.3017f, 0.9024f, -0.3076f, 0.1217f, 0.9379f, -0.3249f, -0.0642f, 0.9403f, -0.3344f,
  -0.2573f, 0.9026f, -0.3451f, -0.4298f, 0.8296f, -0.3564f, -0.5881f, 0.7285f, -0.3513f,
  -0.7242f, 0.6033f, -0.3339f, -0.8355f, 0.4593f, -0.3016f, -0.9166f, 0.2949f, -0.2699f,
  -0.9586f, 0.1169f, -0.2595f, -0.9608f, -0.0717f, -0.2678f, -0.9267f, -0.2561f, -0.2751f,
  -0.8591f, -0.4316f, -0.2750f, -0.7242f, -0.6256f, -0.2900f, -0.5824f, -0.7481f, -0.3179f,
  -0.4202f, -0.8424f, -0.3373f, -0.2436f, -0.9061f, -0.3459f, -0.0603f, -0.9382f, -0.3408f,
  0.1261f, -0.9352f, -0.3308f, 0.3060f, -0.8955f, -0.3232f, 0.4731f, -0.8232f, -0.3138f,
  0.6225f, -0.7212f, -0.3038f, 0.7506f, -0.5952f, -0.2869f, 0.8495f, -0.4467f, -0.2808f,
  0.9146f, -0.2756f, -0.2958f, 0.9491f, -0.0934f, -0.3009f, 0.8918f, -0.0038f, -0.4524f,
  0.8745f, 0.1782f, -0.4510f, 0.8243f, 0.3525f, -0.4430f, 0.7468f, 0.5142f, -0.4218f,
  0.5497f, 0.7288f, -0.4084f, 0.3890f, 0.8059f, -0.4463f, 0.2158f, 0.8573f, -0.4675f,
  0.0351f, 0.8779f, -0.4776f, -0.1472f, 0.8656f, -0.4785f, -0.3157f, 0.8066f, -0.4998f,
  -0.4791f, 0.7208f, -0.5009f, -0.6228f, 0.6079f, -0.4925f, -0.7457f, 0.4750f, -0.4672f,
  -0.8420f, 0.3280f, -0.4283f, -0.9100f, 0.0328f, -0.4132f, -0.8917f, -0.1498f, -0.4271f,
  -0.8425f, -0.3274f, -0.4277f, -0.7615f, -0.4953f, -0.4181f, -0.6342f, -0.6289f, -0.4499f,
  -0.4834f, -0.7350f, -0.4755f, -0.3169f, -0.8117f, -0.4905f, -0.1407f, -0.8580f, -0.4940f,
  0.0377f, -0.8761f, -0.4807f, 0.2114f, -0.8547f, -0.4741f, 0.3803f, -0.7969f, -0.4693f,
  0.5375f, -0.7025f, -0.4665f, 0.6776f, -0.5819f, -0.4497f, 0.8185f, -0.3680f, -0.4413f,
  0.8713f, -0.1889f, -0.4529f, 0.7953f, 0.0963f, -0.5985f, 0.7528f, 0.2746f, -0.5983f,
  0.6822f, 0.4430f, -0.5817f, 0.6178f, 0.5986f, -0.5099f, 0.4650f, 0.6809f, -0.5658f,
  0.2985f, 0.7436f, -0.5983f, 0.1222f, 0.7824f, -0.6107f, -0.0614f, 0.7903f, -0.6097f,
  -0.3567f, 0.6865f, -0.6336f, -0.5061f, 0.5810f, -0.6374f, -0.6391f, 0.4568f, -0.6188f,
  -0.7478f, 0.3136f, -0.5851f, -0.8418f, 0.1722f, -0.5116f, -0.8203f, 0.0059f, -0.5719f,
  -0.7922f, -0.1681f, -0.5866f, -0.7401f, -0.3373f, -0.5818f, -0.6582f, -0.4915f, -0.5703f,
  -0.5262f, -0.6046f, -0.5980f, -0.3712f, -0.6906f, -0.6207f, -0.1995f, -0.7480f, -0.6329f,
  -0.0208f, -0.7829f, -0.6218f, 0.2446f, -0.7483f, -0.6167f, 0.4150f, -0.6786f, -0.6060f,
  0.5662f, -0.5706f, -0.5949f, 0.6984f, -0.4399f, -0.5646f, 0.7645f, -0.2684f, -0.5860f,
  0.8020f, -0.0863f, -0.5911f, 0.6844f, 0.0208f, -0.7288f, 0.6483f, 0.1988f, -0.7350f,
  0.5862f, 0.3689f, -0.7213f, 0.5332f, 0.5322f, -0.6576f, 0.3700f, 0.6038f, -0.7060f,
  0.1966f, 0.6558f, -0.7289f, 0.0182f, 0.6854f, -0.7279f, -0.1882f, 0.6927f, -0.6962f,
  -0.3637f, 0.5459f, -0.7548f, -0.5079f, 0.4274f, -0.7479f, -0.6282f, 0.2860f, -0.7236f,
  -0.7351f, 0.1440f, -0.6625f, -0.7059f, -0.0458f, -0.7068f, -0.6644f, -0.2277f, -0.7118f,
  -0.5713f, -0.4081f, -0.7121f, -0.4290f, -0.5294f, -0.7319f, -0.2595f, -0.6118f, -0.7473f,
  -0.0793f, -0.6596f, -0.7474f, 0.1026f, -0.6888f, -0.7177f, 0.2692f, -0.6140f, -0.7420f,
  0.4346f, -0.5387f, -0.7218f, 0.5758f, -0.4205f, -0.7011f, 0.6855f, -0.1599f, -0.7103f,
  0.5301f, 0.0988f, -0.8422f, 0.4757f, 0.2736f, -0.8360f, 0.4287f, 0.4454f, -0.7860f,
  0.2585f, 0.5067f, -0.8225f, 0.0799f, 0.5481f, -0.8326f, -0.1008f, 0.5805f, -0.8080f,
  -0.2288f, 0.4677f, -0.8538f, -0.3716f, 0.3593f, -0.8560f, -0.4950f, 0.2283f, -0.8384f,
  -0.6093f, 0.0926f, -0.7875f, -0.5640f, -0.0849f, -0.8214f, -0.5148f, -0.2643f, -0.8156f,
  -0.3011f, -0.4559f, -0.8376f, -0.1277f, -0.5123f, -0.8493f, 0.0517f, -0.5539f, -0.8310f,
  0.2172f, -0.4754f, -0.8525f, 0.4307f, -0.3848f, -0.8164f, 0.5611f, -0.2618f, -0.7853f,
  0.5595f, -0.0812f, -0.8249f, 0.3995f, -0.0151f, -0.9166f, 0.3520f, 0.1631f, -0.9217f,
  0.3091f, 0.3404f, -0.8880f, 0.1306f, 0.3893f, -0.9118f, -0.0507f, 0.4316f, -0.9006f,
  -0.1868f, 0.3077f, -0.9330f, -0.3312f, 0.1894f, -0.9244f, -0.4560f, 0.0549f, -0.8883f,
  -0.4023f, -0.1184f, -0.9078f, -0.3499f, -0.2911f, -0.8904f, -0.1761f, -0.3457f, -0.9217f,
  0.0026f, -0.3973f, -0.9177f, 0.2887f, -0.3158f, -0.9039f, 0.4243f, -0.1950f, -0.8843f,
  0.2214f, 0.0390f, -0.9744f, 0.1763f, 0.2173f, -0.9600f, -0.0050f, 0.2658f, -0.9640f,
  -0.1406f, 0.1369f, -0.9806f, -0.2792f, 0.0128f, -0.9601f, -0.2287f, -0.1679f, -0.9589f,
  -0.0508f, -0.2197f, -0.9742f, 0.1249f, -0.2661f, -0.9558f, 0.2621f, -0.1390f, -0.9550f,
  0.0850f, -0.0897f, -0.9923f, 0.0412f, 0.0897f, -0.9951f, -0.0958f, -0.0408f, -0.9946f,
};

} // namespace

// ---------------- 四元数 (§3.6) ----------------

SphereScene::Quat SphereScene::QuatAxisAngle(V3 axis, float angle) {
    float s = std::sin(angle * 0.5f);
    return Quat{ axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5f) };
}

SphereScene::Quat SphereScene::QuatMul(Quat a, Quat b) {
    return Quat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

SphereScene::V3 SphereScene::QuatRotate(Quat q, V3 v) {
    V3 u{ q.x, q.y, q.z };
    V3 t = Cross(u, v) * 2.0f;
    return v + t * q.w + Cross(u, t);
}

SphereScene::Quat SphereScene::QuatFromZTo(V3 n) {
    n = Normalize(n);
    V3 axis{ -n.y, n.x, 0.0f };            // Z × n 定轴
    float asq = Dot(axis, axis);
    float nz = Clampf(n.z, -1.0f, 1.0f);   // acos(Z·n) 定角
    if (asq < 1e-12f) return Quat{ 0.0f, 0.0f, 0.0f, 1.0f };
    return QuatAxisAngle(axis * (1.0f / std::sqrt(asq)), std::acos(nz));
}

// ---------------- 视图基与拾取 (§8.2) ----------------

void SphereScene::ViewBasis(V3& fwd, V3& right, V3& up) const {
    V3 d = camTarget_ - camPos_;
    // 退化保护 (仿 three.js Matrix4.lookAt): 相机落在 target 上时取默认朝向 (0,0,-1);
    // spec §2.3.3 的 savedClick 初值 (0,0,0) 回程可达此状态。
    fwd = Dot(d, d) < 1e-12f ? V3{ 0.0f, 0.0f, -1.0f } : Normalize(d);
    V3 r = Cross(fwd, V3{ 0.0f, 1.0f, 0.0f });
    if (Dot(r, r) < 1e-10f) r = Cross(fwd, V3{ 0.0f, 0.0f, 1.0f });   // 相机正对 ±Y
    right = Normalize(r);
    up = Cross(right, fwd);
}

bool SphereScene::HitsBall(float x, float y) const {   // §8.2 悬停与点击共用
    float w = DipW();
    float h = DipH();
    if (w <= 0.0f || h <= 0.0f) return false;
    float ndcX = (x / w) * 2.0f - 1.0f;
    float ndcY = 1.0f - (y / h) * 2.0f;       // 屏幕 Y 向下 → NDC 取反
    V3 fwd, right, up;
    ViewBasis(fwd, right, up);
    float tanHalf = 1.0f / f_;                // tan(fov/2) = tan25° (f = 1/tan25°)
    V3 dir = Normalize(right * (ndcX * tanHalf * aspect_) +
                       up * (ndcY * tanHalf) + fwd);
    // 解析射线 vs 半径 r=2·sphereScale 球壳 (设置: 拾取半径随球形大小同步,
    // 与投影阶段世界坐标×scale 等价): |camPos + t·dir|² = r² → t² + 2b·t + c = 0
    // pickMesh 为 DoubleSide → 任一 t ≥ 0 的交点即命中 (源码三角化差异 ≤0.0024mm 不可见)
    const float rPick = kRadius * sphereScale_;
    float b = Dot(camPos_, dir);
    float c = Dot(camPos_, camPos_) - rPick * rPick;
    float disc = b * b - c;
    if (disc < 0.0f) return false;
    return -b + std::sqrt(disc) >= 0.0f;
}

// ---------------- 事件入口 (§1.2 / §8.3 / §8.4) ----------------

bool SphereScene::ControlsEnabled() const {   // B15: 悬停 / 点击 / 飞行中禁用输入
    return !hover_ && !clickActive_ && !flight_.active;
}

void SphereScene::PointerMove(float x, float y, bool buttonsDown) {
    if (!initialized_) return;

    // 拖拽旋转 (OrbitControls 自己的 pointermove; enabled 作用面见 spec §1.4)
    if (buttonsDown && ControlsEnabled()) {
        float hh = DipH();
        if (hh > 0.0f) {
            float dx = x - lastX_;
            float dy = y - lastY_;
            // rotateSpeed 0.65: 角度 = 2π × 位移 / 画布高 (OrbitControls rotateLeft/rotateUp)
            float ang = kTwoPi / hh * kRotateSpeed;
            orbitThetaDelta_ -= dx * ang;
            orbitPhiDelta_ -= dy * ang;
        }
    }
    lastX_ = x;
    lastY_ = y;

    if (clickActive_) return;                          // B1 点击态期间 hover 不可改写
    bool hit = HitsBall(x, y);
    if (hit && !hover_ && !buttonsDown) EnterHover();  // B2 进入仅限未按键 (拖拽不打断)
    else if (!hit && hover_) ExitHover();              // B3 退出任何时候允许
}

void SphereScene::PointerDown(float x, float y) {
    pointerDown_ = true;                               // 无条件记录 (§1.2)
    downX_ = x;
    downY_ = y;
    lastX_ = x;                                        // 拖拽增量基准 (避免首帧跳变)
    lastY_ = y;
}

void SphereScene::PointerUp(float x, float y) {        // click 判定 (Viewport.ts:233-245)
    if (!initialized_) return;
    bool down = pointerDown_;
    float dx = x - downX_;
    float dy = y - downY_;
    pointerDown_ = false;
    lastX_ = x;
    lastY_ = y;
    if (down && std::sqrt(dx * dx + dy * dy) > kClickSlop) return;   // B5 拖拽不算点击 (严格 >5px)
    if (!clickEffectEnabled_) return;                  // 设置: 点击效果关 → 不切换点击态
    if (clickActive_) { ReverseClick(); return; }      // B6 已在点击态 → 倒放
    if (!HitsBall(x, y)) return;                      // B7 未命中球壳 → 忽略
    EnterClick();                                      // §1.3 进入点击态
}

void SphereScene::Wheel(int deltaY) {
    if (!initialized_) return;
    if (deltaY < 0) return;                           // B8 上滑开抽屉由 main 负责 (§8.5)
    if (!clickEffectEnabled_) return;                  // 设置: 点击效果关 → 无点击态可倒放
    if (deltaY <= 0 || !clickActive_) return;          // B9 非点击态下滑无动作 (缩放恒关)
    ReverseClick();                                    // §8.4 下滑 → 点击态倒放
}

bool SphereScene::ClickActive() const { return clickActive_; }
bool SphereScene::HoverActive() const { return hover_; }

// ================= 设置集成 (Agent-Heart 设置窗口) =================

void SphereScene::SetAnimationsEnabled(bool on) {
    animationsEnabled_ = on;                       // false: Tick 冻结时钟但仍绘制
}

void SphereScene::SetSphereScale(float s) {
    if (!(s > 0.0f)) s = 1.0f;                     // NaN / 非法值 -> 默认
    if (s < 0.5f) s = 0.5f;                        // 范围 0.5 ~ 1.5
    if (s > 1.5f) s = 1.5f;
    sphereScale_ = s;
}

void SphereScene::SetClickEffectEnabled(bool on) {
    if (clickEffectEnabled_ == on) return;
    clickEffectEnabled_ = on;
    if (!on && clickActive_) ReverseClick();        // 关闭即退出四环互换态 (不卡态)
}

void SphereScene::SetTrailEnabled(bool on) {
    if (trailEnabled_ == on) return;
    trailEnabled_ = on;
    if (!on) trailPts_.clear();                    // 关闭即清空拖尾
}

void SphereScene::SetTrailPoint(float x, float y) {
    if (!trailEnabled_ || !animationsEnabled_) return;   // 关闭 / 动效冻结: 不新增点
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    const unsigned long long now = GetTickCount64();
    if (!trailPts_.empty()) {                      // 光标静止 (位移 ≤1.5px) 不新增点
        const float dx = x - trailPts_.back().x;
        const float dy = y - trailPts_.back().y;
        if (dx * dx + dy * dy < 2.25f) return;
    }
    trailPts_.push_back(TrailPt{ x, y, now });
    while (trailPts_.size() > 40)                  // 上限约 40 点
        trailPts_.erase(trailPts_.begin());
}

void SphereScene::DrawTrail(ID2D1DeviceContext* dc) {
    if (!dc || !trailEnabled_ || !animationsEnabled_ || trailPts_.empty()) return;
    constexpr unsigned long long kLifeMs = 600;         // 寿命约 0.6 秒
    const unsigned long long now = GetTickCount64();

    size_t keep = 0;                                     // 剔除过期点 (保持时序)
    for (size_t i = 0; i < trailPts_.size(); ++i)
        if (now - trailPts_[i].tMs <= kLifeMs) trailPts_[keep++] = trailPts_[i];
    trailPts_.resize(keep);
    if (trailPts_.empty()) return;

    // 白偏淡蓝; Draw 顶部已设 D2D1_PRIMITIVE_BLEND_ADD -> 黑底发光叠加
    const D2D1_COLOR_F base{ 0.78f, 0.88f, 1.0f, 1.0f };
    winrt::com_ptr<ID2D1SolidColorBrush> br;
    dc->CreateSolidColorBrush(&base, nullptr, br.put());
    if (!br) return;

    for (size_t i = 1; i < trailPts_.size(); ++i) {      // 渐隐折线
        const float life = 1.0f - static_cast<float>(now - trailPts_[i].tMs) /
                                  static_cast<float>(kLifeMs);
        if (life <= 0.0f) continue;
        const D2D1_COLOR_F c{ base.r, base.g, base.b, life * life };   // alpha 随寿命衰减
        br->SetColor(&c);
        const float width = 1.5f + 1.0f * life;          // 粗细 1.5 ~ 2.5px
        const D2D1_POINT_2F p0{ trailPts_[i - 1].x, trailPts_[i - 1].y };
        const D2D1_POINT_2F p1{ trailPts_[i].x, trailPts_[i].y };
        dc->DrawLine(p0, p1, br.get(), width);
    }
    for (size_t i = 0; i < trailPts_.size(); ++i) {      // 端点圆点 (新点更亮更大)
        const float life = 1.0f - static_cast<float>(now - trailPts_[i].tMs) /
                                  static_cast<float>(kLifeMs);
        if (life <= 0.0f) continue;
        const D2D1_COLOR_F c{ base.r, base.g, base.b, life * 0.8f };
        br->SetColor(&c);
        const float r = 1.0f + 1.2f * life;
        const D2D1_ELLIPSE el{ D2D1_POINT_2F{ trailPts_[i].x, trailPts_[i].y }, r, r };
        dc->FillEllipse(&el, br.get());
    }
}

// ---------------- 相机飞行 (§1.3 / §2) ----------------

void SphereScene::StartFlight(V3 toPos, V3 toTarget, float duration) {
    // 无条件覆盖现有 flight; from = 调用瞬间位姿 (半路即半路), §2.1
    flight_.active = true;
    flight_.fromPos = camPos_;
    flight_.fromTarget = camTarget_;
    flight_.toPos = toPos;
    flight_.toTarget = toTarget;
    flight_.elapsed = 0.0f;
    flight_.duration = duration;
}

void SphereScene::EnterHover() {
    if (!flight_.active) {                            // B13 飞行途中不覆盖 saved (注释 :298)
        savedPos_ = camPos_;
        savedTarget_ = camTarget_;
    }
    hover_ = true;
    morphActive_ = true;                              // → setMorph(true)
    float d = Clampf(Length(camPos_ - camTarget_), kHoverMinDist, kHoverMaxDist);
    StartFlight(V3{ 0.0f, 0.0f, d }, V3{ 0.0f, 0.0f, 0.0f }, kHoverFlyTime);
}

void SphereScene::ExitHover() {
    hover_ = false;
    morphActive_ = false;                             // → setMorph(false)
    StartFlight(savedPos_, savedTarget_, kHoverFlyTime);   // from = 当前位姿 (可为半路)
}

void SphereScene::EnterClick() {
    if (clickActive_) return;                         // B10 防重入
    clickActive_ = true;
    if (!flight_.active) {                            // B11 记点击前位姿
        savedClickPos_ = camPos_;
        savedClickTarget_ = camTarget_;
    } else if (hover_) {                              // B12 飞行半路点击 → 取悬停最初位姿
        savedClickPos_ = savedPos_;
        savedClickTarget_ = savedTarget_;
    }
    // 第三种情形 (flight 中且非 hover) 不写 savedClick → 保持初值 (0,0,0), spec §2.3.3
    float d = Clampf(Length(camPos_ - camTarget_), kHoverMinDist, kHoverMaxDist);
    StartFlight(V3{ d, 0.0f, 0.0f }, V3{ 0.0f, 0.0f, 0.0f }, kClickFlyTime);
    // 不清 hover、不动 morph (§2.3.4): 点击态可与悬停态并存
}

void SphereScene::ReverseClick() {
    if (!clickActive_) return;                        // B14 防重入
    clickActive_ = false;
    StartFlight(savedClickPos_, savedClickTarget_, kClickFlyTime);
}

// ---------------- 每帧相机 (§1.4) ----------------

void SphereScene::UpdateCamera(double dt) {
    if (flight_.active) {                             // B16 直线插值 (可穿球心)
        flight_.elapsed += static_cast<float>(dt);
        float t = flight_.duration > 0.0f ? flight_.elapsed / flight_.duration : 1.0f;
        if (t > 1.0f) t = 1.0f;
        float e = EaseInOutCubic(t);
        camPos_ = flight_.fromPos + (flight_.toPos - flight_.fromPos) * e;
        camTarget_ = flight_.fromTarget + (flight_.toTarget - flight_.fromTarget) * e;
        if (t >= 1.0f) flight_.active = false;
        return;
    }
    if (hover_) return;                               // B17 位置锁死 (朝向由 ViewBasis 每帧保证)
    ControlsUpdate();                                 // B18 阻尼旋转 (点击态仅衰减, 无输入增量)
}

void SphereScene::ControlsUpdate() {                  // §3.2 OrbitControls 球坐标 θ/φ/r
    if (orbitThetaDelta_ == 0.0f && orbitPhiDelta_ == 0.0f) return;   // 无输入 → 位姿不动 (N1 恒等)
    V3 off = camPos_ - camTarget_;
    float r = Length(off);
    if (r < 1e-9f) r = 1e-9f;
    float theta = std::atan2(off.x, off.z);
    float phi = std::acos(Clampf(off.y / r, -1.0f, 1.0f));
    theta += orbitThetaDelta_ * kDamping;             // 每帧只吃 8% 增量
    phi += orbitPhiDelta_ * kDamping;
    phi = Clampf(phi, 0.0f, kMaxPolar);               // maxPolarAngle = π/2 − 0.02
    r = Clampf(r, kMinOrbitDist, kMaxOrbitDist);      // 1.5 ~ 120 (zoom 禁用)
    camPos_ = camTarget_ + V3{ r * std::sin(phi) * std::sin(theta),
                               r * std::cos(phi),
                               r * std::sin(phi) * std::cos(theta) };
    orbitThetaDelta_ *= kDampDecay;                   // 阻尼衰减 ×0.92
    orbitPhiDelta_ *= kDampDecay;
}

// ---------------- 形变初值 (§3.3 / §3.6b / §5.3) ----------------

void SphereScene::InitBallFromSites(int hexCount) {
    // 表的唯一消费点 (SphereParticles.ts:566-585): 站点 → 球坐标, 只保证开局不聚堆
    if (hexCount > 0 && hexCount != 405) {
        OutputDebugStringA("[sphere] hex sites count mismatch (expect 405)\n");
    }
    int siteRank = 0;
    for (int i = 0; i < kParticles; ++i) {
        if (shapeKind_[i] != 2) continue;
        int s = (siteRank % 405) * 3;
        ++siteRank;
        float x = kHexSites[s];
        float y = kHexSites[s + 1];
        float z = kHexSites[s + 2];
        float len = std::sqrt(x * x + y * y + z * z);
        if (len <= 0.0f) len = 1.0f;                  // Math.hypot(x,y,z) || 1
        ballPath_[i] = std::atan2(y / len, x / len);  // φ
        ballTube_[i] = std::acos(Clampf(z / len, -1.0f, 1.0f));   // θ
    }
}

void SphereScene::InitMorphState() {
    disp_.assign(kParticles, V3{ 0.0f, 0.0f, 0.0f });
    morphTarget_.assign(kParticles, V3{ 0.0f, 0.0f, 0.0f });
    clickTarget_.assign(kParticles, V3{ 0.0f, 0.0f, 0.0f });
    morphDelay_.assign(kParticles, 0.0f);
    shapePath_.assign(kParticles, 0.0f);
    shapePathSpeed_.assign(kParticles, 0.0f);
    shapePathAim_.assign(kParticles, 0.0f);
    shapeTube_.assign(kParticles, 0.0f);
    shapeTubeSpeed_.assign(kParticles, 0.0f);
    shapeTubeAim_.assign(kParticles, 0.0f);
    shapeHold_.assign(kParticles, 0.0f);
    middleZ_.assign(kParticles, 0.0f);
    ballPath_.assign(kParticles, 0.0f);
    ballTube_.assign(kParticles, 0.0f);
    shapeKind_.assign(kParticles, 0);
    ringOf_.assign(kParticles, static_cast<signed char>(-1));
    focus_.assign(kParticles, 0.0f);
    hexAngle_ = 0.0f;
    torusAngle_ = 0.0f;
    gimbalOrientation_ = Quat{ 0.0f, 0.0f, 0.0f, 1.0f };

    // §3.6a: 4 只环固定相对姿态 = 正四面体 4 条体对角线 (两两 70.53°)
    static const V3 kRingNormals[4] = {
        V3{ 1.0f, 1.0f, 1.0f }, V3{ 1.0f, -1.0f, -1.0f },
        V3{ -1.0f, 1.0f, -1.0f }, V3{ -1.0f, -1.0f, 1.0f },
    };
    for (int k = 0; k < kRingCount; ++k) {
        gimbalFixedLocal_[k] = QuatFromZTo(kRingNormals[k]);
        ringQuaternion_[k] = Quat{ 0.0f, 0.0f, 0.0f, 1.0f };
    }

    float pathSpan = kRadius * (1.0f + 2.0f * kHexMiterR);   // hexPathSpan = 1.08r
    for (int i = 0; i < kParticles; ++i) {
        morphDelay_[i] = Rand() * kMorphStagger;             // rand(0, 0.35)
        shapeHold_[i] = RandBetween(kMinHold, kMaxHold);      // rand(3, 8)
        shapeTubeAim_[i] = RandBetween(-kShapeTubeSpeed, kShapeTubeSpeed);

        int local = i % kPerLayer;
        int shape = local < 81 ? 1 : (local < 216 ? 2 : 3);   // initialShape (§3.3): 243/405/120
        shapeKind_[i] = static_cast<unsigned char>(shape);

        if (shape == 1) {                                     // 甜甜圈: 表面均匀采样
            float major = kRadius * kTorusMajorR;
            float minor = kRadius * kTorusMinorR;
            float tubeAngle = 0.0f;
            for (int attempt = 0; attempt < 8; ++attempt) {   // 拒绝采样最多 8 次
                tubeAngle = Rand() * kTwoPi;
                if (major + minor * std::cos(tubeAngle) >= Rand() * (major + minor)) break;
            }
            shapePath_[i] = Rand() * kTwoPi;                  // 主角 φ
            shapeTube_[i] = tubeAngle;                        // 管角 θ
            shapePathAim_[i] = RandBetween(-kTorusPathSpeed, kTorusPathSpeed);
        } else if (shape == 2) {                              // 六边形管
            shapeTube_[i] = Rand() * kTwoPi;
            shapePath_[i] = Rand() * (6.0f * pathSpan);
            shapePathAim_[i] = RandBetween(-kHexPathSpeed, kHexPathSpeed);
        } else {                                              // 中间层
            shapePath_[i] = Rand() * kTwoPi;                  // 公转相位
            shapeTube_[i] = Rand() * kTwoPi;                  // 径向呼吸相位
            shapePathAim_[i] = RandBetween(-kMiddlePathSpeed, kMiddlePathSpeed);
            middleZ_[i] = RandBetween(-1.0f, 1.0f) * kRadius * kMiddleHeightR;
        }
    }

    // §3.6(b) 环分配: 非六边形 363 颗全要 + 六边形组按 i 升序征用前 117 颗 → 每环恰 120
    int nonHex = 0;
    for (int i = 0; i < kParticles; ++i)
        if (shapeKind_[i] != 2) ++nonHex;
    int extraNeeded = 480 - nonHex;
    if (extraNeeded < 0) extraNeeded = 0;
    int taken = 0;
    int rank = 0;
    for (int i = 0; i < kParticles; ++i) {
        bool isRing = shapeKind_[i] != 2;
        if (!isRing && taken < extraNeeded) { isRing = true; ++taken; }
        if (isRing) {
            ringOf_[i] = static_cast<signed char>(rank % kRingCount);
            ++rank;
        }
    }

    int hexCount = 0;
    for (int i = 0; i < kParticles; ++i)
        if (shapeKind_[i] == 2) ++hexCount;
    InitBallFromSites(hexCount);

    BlendMorph();                                             // 构造末: 首帧显示坐标
    SyncBuffers();
}

// ---------------- 每帧推进 (§5.4) ----------------

void SphereScene::ParticleUpdate(double dt) {
    if (dt <= 0.0) return;                                    // :640
    float fd = static_cast<float>(dt);
    morphClock_ = morphActive_ ? std::min(kMorphTotal, morphClock_ + fd)
                               : std::max(0.0f, morphClock_ - fd);   // B19 ∈ [0, 1.1]
    clickClock_ = clickActive_ ? std::min(kClickSwapDur, clickClock_ + fd)
                               : std::max(0.0f, clickClock_ - fd);   // B20 ∈ [0, 1.0]
    Simulate(dt);                                            // 恒运行 (自由漂移, elapsed_ 在内推进)
    bool swap = clickClock_ > 0.0f;                           // :660
    if (swap || morphClock_ > 0.0f) {                         // B21 含退出途中, 每帧重算目标
        UpdateShapeMotion(fd, swap);
        UpdateTargets(fd, swap);
    }
    BlendMorph();                                             // §3.7
    SyncBuffers();                                            // §3.8
}

// ---------------- 形态游离 (§3.2) ----------------

void SphereScene::UpdateShapeMotion(float dt, bool swap) {
    float decay = 1.0f - std::exp(-dt / kAimTau);
    float loopLength = 6.0f * kRadius * (1.0f + 2.0f * kHexMiterR);   // 6 × hexPathSpan

    for (int i = 0; i < kParticles; ++i) {
        int shape = shapeKind_[i];

        shapeHold_[i] -= dt;
        if (shapeHold_[i] <= 0.0f) {                          // 每 3~8 s 重取目标速度
            shapeHold_[i] = RandBetween(kMinHold, kMaxHold);
            float pathMax = shape == 1 ? kTorusPathSpeed
                         : shape == 2 ? (swap ? kBallPathSpeed : kHexPathSpeed)
                                      : kMiddlePathSpeed;
            shapePathAim_[i] = RandBetween(-pathMax, pathMax);
            float tubeMax = (swap && shape == 2) ? kBallTubeSpeed : kShapeTubeSpeed;
            shapeTubeAim_[i] = RandBetween(-tubeMax, tubeMax);
        }
        shapePathSpeed_[i] += (shapePathAim_[i] - shapePathSpeed_[i]) * decay;
        shapeTubeSpeed_[i] += (shapeTubeAim_[i] - shapeTubeSpeed_[i]) * decay;

        if (swap && shape == 2) {                             // 点击态球面点 (405 颗共用)
            ballPath_[i] = WrapPeriod(ballPath_[i] + shapePathSpeed_[i] * dt, kTwoPi);
            float theta = ballTube_[i] + shapeTubeSpeed_[i] * dt;
            if (theta < 0.0f) {                               // 极点反弹
                theta = -theta;
                shapeTubeSpeed_[i] = std::fabs(shapeTubeSpeed_[i]);
            } else if (theta > kPi) {
                theta = kTwoPi - theta;
                shapeTubeSpeed_[i] = -std::fabs(shapeTubeSpeed_[i]);
            }
            ballTube_[i] = theta;                             // 该组原生参数此帧冻结
        } else {
            shapePath_[i] += shapePathSpeed_[i] * dt;
            shapeTube_[i] += shapeTubeSpeed_[i] * dt;
            float pathPeriod = shape == 2 ? loopLength : kTwoPi;
            shapePath_[i] = WrapPeriod(shapePath_[i], pathPeriod);
            shapeTube_[i] = WrapPeriod(shapeTube_[i], kTwoPi);
        }
    }
}

// ---------------- 目标点重算 (§3.4~§3.6) ----------------

void SphereScene::UpdateTargets(float dt, bool swap) {
    float radius = kRadius;
    float major = radius * kTorusMajorR;
    float minor = radius * kTorusMinorR;
    float tubeR = radius * kHexTubeR;
    float side = radius;                                     // 正六边形边长 = 外接圆半径
    float miter = radius * kHexMiterR;
    float pathSpan = side + 2.0f * miter;                    // 1.08r
    float middleInner = radius * kMiddleInnerR;
    float middleOuter = radius * kMiddleOuterR;
    float middleCenter = (middleInner + middleOuter) * 0.5f; // 0.6625r
    float middleAmp = (middleOuter - middleInner) * 0.5f;    // 0.0875r

    hexAngle_ += kHexSpinRad * dt;                           // −6°/s 从 +Z 看顺时针
    torusAngle_ += kTorusTumbleRad * dt;                     // +14°/s
    float e = static_cast<float>(elapsed_);
    V3 axis = Normalize(V3{ kTorusAxisTilt * std::sin(e * kTorusDriftX),
                            kTorusAxisTilt * std::cos(e * kTorusDriftY), 1.0f });
    Quat torusQuat = QuatAxisAngle(axis, torusAngle_);
    float cosHex = std::cos(hexAngle_);
    float sinHex = std::sin(hexAngle_);

    // ---- §3.5 三组基准目标 (每帧重算, 汇聚完成后画面仍在动) ----
    for (int i = 0; i < kParticles; ++i) {
        int shape = shapeKind_[i];
        if (shape == 1) {                                    // 甜甜圈: 管面局部坐标 → 整体翻滚
            float phi = shapePath_[i];
            float theta = shapeTube_[i];
            float ring = major + minor * std::cos(theta);
            V3 local{ ring * std::cos(phi), ring * std::sin(phi), minor * std::sin(theta) };
            morphTarget_[i] = QuatRotate(torusQuat, local);
        } else if (shape == 2) {                             // 六边形管: 周长定位 + 圆管截面 + 绕 Z
            float along = shapePath_[i];
            float theta = shapeTube_[i];
            int edge = static_cast<int>(std::floor(along / pathSpan)) % 6;
            float t = along - static_cast<float>(edge) * pathSpan - miter;
            float angleA = static_cast<float>(edge) * kPi / 3.0f;
            float angleB = angleA + kPi / 3.0f;
            float ax = radius * std::cos(angleA);
            float ay = radius * std::sin(angleA);
            float dirX = (radius * std::cos(angleB) - ax) / side;
            float dirY = (radius * std::sin(angleB) - ay) / side;
            float nx = -dirY;
            float ny = dirX;
            float lx = ax + dirX * t + tubeR * std::cos(theta) * nx;
            float ly = ay + dirY * t + tubeR * std::cos(theta) * ny;
            float lz = tubeR * std::sin(theta);
            morphTarget_[i] = V3{ lx * cosHex - ly * sinHex,
                                  lx * sinHex + ly * cosHex, lz };
        } else {                                             // 中间层: 半径呼吸 + 固定高度
            float phi = shapePath_[i];
            float rr = middleCenter + middleAmp * std::sin(shapeTube_[i]);
            morphTarget_[i] = V3{ rr * std::cos(phi), rr * std::sin(phi), middleZ_[i] };
        }
    }

    if (!swap) return;                                       // §3.5 早退: 非点击态不算 clickTarget

    // ---- §3.6(a) 整体随机自转: 轴漂移 + 转速起伏, 世界系累乘 ----
    V3 gAxis = Normalize(V3{ std::sin(e * kAssemblyDriftA),
                             std::cos(e * kAssemblyDriftB),
                             0.6f + 0.4f * std::sin(e * kAssemblyDriftC) });
    float spinMod = 1.0f + kAssemblySpinWave * std::sin(e * 0.17f);
    Quat q = QuatAxisAngle(gAxis, kAssemblySpinRad * spinMod * dt);
    gimbalOrientation_ = QuatMul(q, gimbalOrientation_);      // premultiply = q × 当前姿态
    for (int k = 0; k < kRingCount; ++k)                      // 4 环相对姿态锁死
        ringQuaternion_[k] = QuatMul(gimbalOrientation_, gimbalFixedLocal_[k]);

    float ringMajor = radius * kRingMajorR;
    float ringMinor = radius * kRingMinorR;
    float ballRadius = radius * kBallRadiusR;

    for (int i = 0; i < kParticles; ++i) {
        int ring = ringOf_[i];
        if (ring >= 0) {                                      // §3.6(c) 环上点 (480 颗)
            bool fromHex = shapeKind_[i] == 2;
            float phi = fromHex ? ballPath_[i] : shapePath_[i];
            float theta = fromHex ? ballTube_[i] * 2.0f : shapeTube_[i];   // 球极角 [0,π] → [0,2π]
            float rr = ringMajor + ringMinor * std::cos(theta);
            V3 local{ rr * std::cos(phi), rr * std::sin(phi), ringMinor * std::sin(theta) };
            clickTarget_[i] = QuatRotate(ringQuaternion_[ring], local);
        } else {                                              // §3.6(d) Ø1mm 球面点 (288 颗)
            float phi = ballPath_[i];
            float theta = ballTube_[i];
            float st = std::sin(theta);
            clickTarget_[i] = V3{ ballRadius * st * std::cos(phi),
                                  ballRadius * st * std::sin(phi),
                                  ballRadius * std::cos(theta) };
        }
    }
}

// ---------------- 显示坐标与 focus (§3.7 / §3.8) ----------------

void SphereScene::BlendMorph() {
    float swap = clickClock_ / kClickSwapDur;
    if (morphClock_ <= 0.0f && swap <= 0.0f) {                // 快路径: 整段拷贝 (N1 恒走此路)
        for (int i = 0; i < kParticles; ++i) disp_[i] = particles_[i].pos;
        return;
    }
    for (int i = 0; i < kParticles; ++i) {
        V3 base;
        if (morphClock_ <= 0.0f) {                            // 第 1 层: 自由球面 ↔ 三组形态
            base = particles_[i].pos;
        } else {
            float p = EaseInOutCubic((morphClock_ - morphDelay_[i]) / kMorphDuration);
            if (p <= 0.0f) base = particles_[i].pos;
            else if (p >= 1.0f) base = morphTarget_[i];
            else base = particles_[i].pos + (morphTarget_[i] - particles_[i].pos) * p;
        }
        if (swap > 0.0f) {                                    // 第 2 层: 单段缓动, 倒走即整段倒放
            float t = EaseInOutCubic(swap);
            base = base + (clickTarget_[i] - base) * t;
        }
        disp_[i] = base;
    }
}

void SphereScene::SyncBuffers() {                             // §3.8/§4: 显示坐标 → focus
    float swapProgress = clickClock_ / kClickSwapDur;
    float ramp = kRadius * kFocusXRamp;                       // 0.15r = 0.3mm 过渡带
    for (int i = 0; i < kParticles; ++i) {
        float px = disp_[i].x;
        float xBlur = px >= 0.0f ? 0.0f : std::min(1.0f, -px / ramp);   // 按世界 x 切分 (勘误见 §4.3)
        focus_[i] = xBlur * swapProgress;                     // 仅点击态非零 (悬停态恒 0)
    }
}

// ---------------- Stage3: 场景快照 (毛玻璃模糊源) ----------------
bool SphereScene::EnsureSnapshot(ID2D1DeviceContext* dc) {
    if (!dc || pxW_ <= 0 || pxH_ <= 0) return false;
    if (snapshotBmp_) {
        D2D1_SIZE_U sz = snapshotBmp_->GetPixelSize();
        if (sz.width == static_cast<UINT>(pxW_) && sz.height == static_cast<UINT>(pxH_))
            return true;
        snapshotBmp_ = nullptr;
    }
    float dpiX = 96.0f, dpiY = 96.0f;
    dc->GetDpi(&dpiX, &dpiY);
    if (dpiX <= 0.0f) dpiX = 96.0f;
    if (dpiY <= 0.0f) dpiY = 96.0f;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        dpiX, dpiY);
    D2D1_SIZE_U px{ static_cast<UINT>(pxW_), static_cast<UINT>(pxH_) };
    if (FAILED(dc->CreateBitmap(px, nullptr, 0, &props, snapshotBmp_.put()))) {
        snapshotBmp_ = nullptr;
        return false;
    }
    return true;
}
