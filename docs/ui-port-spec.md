# Frontend UI 层动效与交互 · 纯原生 Windows 移植规格（权威）

> 本文档只记录事实：所有数值、颜色、时长、行为均逐条摘自源码，并标注来源行号（格式 `文件:行号`）。
> 不包含任何实现建议。源码路径均相对于仓库 `D:\Projects\AI-system`。

## 0. 源码清单与全局约定

### 0.1 被分析的文件

| 文件 | 行数 | 说明 |
| --- | --- | --- |
| `Frontend/src/Drawer.tsx` | 241 | 应用抽屉组件（逻辑） |
| `Frontend/src/Drawer.css` | 270 | 应用抽屉样式 |
| `Frontend/src/TaskPanel.tsx` | 177 | 任务面板组件（逻辑） |
| `Frontend/src/TaskPanel.css` | 215 | 任务面板样式 |
| `Frontend/src/App.tsx` | 57 | 顶层 overlay 状态与全局轮询 |
| `Frontend/src/styles/global.css` | 44 | 全局重置、字体、视口 |
| `Frontend/index.html` | 13 | 页面壳 |
| `Frontend/src/main.tsx` | 15 | 挂载点（引入 global.css） |
| `Frontend/src/scene/Viewport.ts` | 372 | 三维视口（wheel → onSwipeUp 来源） |
| `Frontend/dist/assets/index-8WrFANw0.css` | 1（压缩单行） | 构建产物，用于对照 |

`Frontend/src` 下只有 3 个 CSS 文件：`styles/global.css`、`Drawer.css`、`TaskPanel.css`（glob `Frontend/src/**/*.css` 结果）。

### 0.2 BRIDGE 常量

- `App.tsx:8` → `const BRIDGE = 'http://127.0.0.1:8787'`
- `Drawer.tsx:4` → 同值
- `TaskPanel.tsx:4` → 同值
  三个文件各自定义了同一个常量，值完全一致。

### 0.3 页面壳（`Frontend/index.html`）

- `index.html:2` `<html lang="zh-CN">`
- `index.html:4` charset UTF-8
- `index.html:5` viewport：`width=device-width, initial-scale=1.0`
- `index.html:6` `<meta name="color-scheme" content="dark" />`
- `index.html:7` `<title>MeSquare · 三维视口</title>`
- `index.html:10` `<div id="root"></div>`
- `index.html:11` `<script type="module" src="/src/main.tsx"></script>`
- 页面不加载任何外部字体、图片或样式表。

### 0.4 全局样式（`Frontend/src/styles/global.css`）

| 选择器 | 属性 | 值 | 来源 |
| --- | --- | --- | --- |
| `:root` | color-scheme | dark | global.css:1-3 |
| `*` | margin / padding | 0 / 0 | global.css:5-7 |
| `*` | box-sizing | border-box（内边距计入宽高） | global.css:8 |
| `html, body, #root` | width / height | 100% / 100% | global.css:14-15 |
| `html, body, #root` | overflow | hidden | global.css:16 |
| `html, body, #root` | background-color | `#000000` | global.css:17 |
| `body` | font-family | 见 §7 | global.css:21-27 |
| `body` | color | `#e8eaed` | global.css:28 |
| `body` | -webkit-font-smoothing | antialiased | global.css:29 |
| `body` | -moz-osx-font-smoothing | grayscale | global.css:30 |
| `.viewport` | position / inset | fixed / 0 | global.css:34-35 |
| `.viewport` | background-color | `#000000` | global.css:36 |
| `.viewport canvas` | display | block | global.css:40 |
| `.viewport canvas` | width / height | 100% / 100% | global.css:41-42 |
| `.viewport canvas` | touch-action | none | global.css:43 |

`.viewport` 无 z-index（auto）。`main.tsx:4` 引入 `./styles/global.css`；`Drawer.css`、`TaskPanel.css` 分别由 `Drawer.tsx:2`、`TaskPanel.tsx:2` 引入。

### 0.5 z-index 层级（由低到高）

| 层 | 选择器 | z-index | position | 来源 |
| --- | --- | --- | --- | --- |
| 三维视口 | `.viewport` | 未声明（auto） | fixed, inset 0 | global.css:33-37 |
| 应用抽屉 | `.drawer-root` | **10** | fixed, inset 0 | Drawer.css:3-5 |
| 任务面板 | `.tp-root` | **20** | fixed, inset 0 | TaskPanel.css:3-5 |

两个 overlay 根节点始终挂载在 DOM 中，仅靠 `.open` 类切换：
- `Drawer.tsx:158` → `className={'drawer-root' + (open ? ' open' : '')} aria-hidden={!open}`
- `TaskPanel.tsx:124` → `className={'tp-root' + (open ? ' open' : '')} aria-hidden={!open}`
- 关闭态：`pointer-events: none`（Drawer.css:6、TaskPanel.css:6）；打开态：`pointer-events: auto`（Drawer.css:10、TaskPanel.css:10）。

---

## 1. 布局几何（逐元素）

### 1.1 应用抽屉（Drawer）

所有尺寸为 CSS 像素；`box-sizing: border-box`（global.css:8）。

#### 根与背景

| 元素/选择器 | 属性 | 值 | 来源 |
| --- | --- | --- | --- |
| `.drawer-root` | position / inset / z-index | fixed / 0 / 10 | Drawer.css:3-5 |
| `.drawer-root` | pointer-events | none（`.open` 时 auto） | Drawer.css:6、10 |
| `.drawer-backdrop` | position / inset | absolute / 0 | Drawer.css:14-15 |
| `.drawer-backdrop`（关闭） | background | `rgba(0, 0, 0, 0)` → `#00000000` | Drawer.css:16 |
| `.drawer-backdrop`（打开） | background | `rgba(0, 0, 0, 0.35)` → `#00000059` | Drawer.css:21（dist 对照 `#00000059`） |
| `.drawer-panel` | position / inset | absolute / 0（全屏） | Drawer.css:25-26 |
| `.drawer-panel` | background | `rgba(10, 11, 14, 0.52)` → `#0a0b0e85` | Drawer.css:27（dist `#0a0b0e85`） |
| `.drawer-panel` | border | `1px solid rgba(255,255,255,0.08)` → `#ffffff14` | Drawer.css:30（dist `#ffffff14`） |
| `.drawer-panel` | box-shadow | `inset 0 1px 0 rgba(255,255,255,0.06)` → `inset 0 1px #ffffff0f` | Drawer.css:31（dist `#ffffff0f`） |
| `.drawer-panel` | border-radius | **未声明（0）** | Drawer.css:24-42 |
| `.drawer-panel` | color | `#e8eaed` | Drawer.css:32 |
| `.drawer-panel` | display / flex-direction | flex / column | Drawer.css:39-40 |
| `.drawer-panel` | touch-action | none | Drawer.css:41 |
| `.drawer-panel` | backdrop-filter | `blur(60px) saturate(1.6)` | Drawer.css:28-29（见 §5） |

#### 头部 `.drawer-head`（拖拽热区）

| 属性 | 值 | 来源 |
| --- | --- | --- |
| position | relative | Drawer.css:54 |
| display / align-items | flex / center | Drawer.css:55-56 |
| gap | 18px | Drawer.css:57 |
| padding | **22px 32px 12px**（上 右 下 左） | Drawer.css:58 |
| border-bottom | `1px solid rgba(255,255,255,0.08)` → `#ffffff14` | Drawer.css:59 |
| flex-shrink | 0 | Drawer.css:60 |
| cursor | grab；`:active` 时 grabbing | Drawer.css:61、64-65 |

**拖拽把手 `.drawer-head::before`**（Drawer.css:69-79）：`content: ''`、absolute、`top: 7px`、`left: 50%`、`transform: translateX(-50%)`（静态）、宽 **46px**、高 **5px**、`border-radius: 3px`、`background: rgba(255,255,255,0.28)` → `#ffffff47`（dist `#ffffff47`）。

#### 标签栏 `.drawer-tabs` / `.drawer-tab`

| 选择器 | 属性 | 值 | 来源 |
| --- | --- | --- | --- |
| `.drawer-tabs` | display / gap | flex / 10px | Drawer.css:82-83 |
| `.drawer-tabs` | flex / overflow-x / padding-bottom | 1 / auto / 2px | Drawer.css:84-86 |
| `.drawer-tab` | background | `rgba(255,255,255,0.07)` → `#ffffff12` | Drawer.css:90 |
| `.drawer-tab` | border | `1px solid rgba(255,255,255,0.13)` → `#ffffff21` | Drawer.css:91 |
| `.drawer-tab` | color | `#c6ccd6` | Drawer.css:92 |
| `.drawer-tab` | padding | 7px 18px | Drawer.css:93 |
| `.drawer-tab` | border-radius | 999px（胶囊） | Drawer.css:94 |
| `.drawer-tab` | font-size | 14px（未声明字重 → 继承 body 默认 400） | Drawer.css:95 |
| `.drawer-tab` | white-space | nowrap | Drawer.css:97 |
| `.drawer-tab:hover` | background | `rgba(255,255,255,0.14)` → `#ffffff24` | Drawer.css:109 |
| `.drawer-tab.active` | background / border-color | `#e8eaed` / `#e8eaed` | Drawer.css:113、115 |
| `.drawer-tab.active` | color / font-weight | `#101216` / 600 | Drawer.css:114、116 |
| `.drawer-tab i`（计数徽标） | font-style / margin-left / font-size / opacity | normal / 7px / 11px / 0.55 | Drawer.css:102-105 |

#### 头部右侧 `.drawer-actions`

| 选择器 | 属性 | 值 | 来源 |
| --- | --- | --- | --- |
| `.drawer-actions` | display / align-items / gap | flex / center / 14px | Drawer.css:120-122 |
| `.drawer-hint` | font-size / color / white-space | 12px / `rgba(255,255,255,0.42)`→`#ffffff6b` / nowrap | Drawer.css:126-128 |
| `.drawer-hint` 文案 | — | `标题栏下拉 · Esc 或 Win+D 关闭` | Drawer.tsx:184 |
| `.drawer-btn` | background | `rgba(255,255,255,0.08)` → `#ffffff14` | Drawer.css:132 |
| `.drawer-btn` | border | `1px solid rgba(255,255,255,0.14)` → `#ffffff24` | Drawer.css:133 |
| `.drawer-btn` | color | `#d5dae2` | Drawer.css:134 |
| `.drawer-btn` | padding / border-radius / font-size | 7px 18px / 10px / 13px | Drawer.css:135-137 |
| `.drawer-btn:hover` | background / border-color | `rgba(255,255,255,0.16)`→`#ffffff29` / `rgba(255,255,255,0.3)`→`#ffffff4d` | Drawer.css:143-144 |
| `.drawer-btn.on`（编辑模式开启） | background / border-color / color | `#e8eaed` / `#e8eaed` / `#101216` | Drawer.css:148-150 |

#### 编辑工具条 `.drawer-editbar`（`edit === true` 时渲染，Drawer.tsx:191-204）

| 选择器 | 属性 | 值 | 来源 |
| --- | --- | --- | --- |
| `.drawer-editbar` | display / align-items / gap | flex / center / 12px | Drawer.css:154-156 |
| `.drawer-editbar` | padding | 12px 32px | Drawer.css:157 |
| `.drawer-editbar` | border-bottom | `1px solid rgba(255,255,255,0.07)` → `#ffffff12` | Drawer.css:158 |
| `.drawer-editbar` | background | `rgba(255,255,255,0.04)` → `#ffffff0a` | Drawer.css:159 |
| `.drawer-editbar input` | background | `rgba(0,0,0,0.35)` → `#00000059` | Drawer.css:164 |
| `.drawer-editbar input` | border | `1px solid rgba(255,255,255,0.16)` → `#ffffff29` | Drawer.css:165 |
| `.drawer-editbar input` | color / padding / radius / font-size | `#e8eaed` / 8px 14px / 10px / 13px | Drawer.css:166-169 |
| `.drawer-editbar input` | width / outline | **260px** / none | Drawer.css:170-171 |
| `::placeholder` | color | `rgba(255,255,255,0.35)` → `#ffffff59` | Drawer.css:175 |
| 输入框文案 | placeholder | `新分类名，如 影音` | Drawer.tsx:195 |
| 按钮文案 | — | `添加分类` | Drawer.tsx:200 |
| 条内提示 | — | `在应用上选择分类即可归类` | Drawer.tsx:202 |

#### 主体 `.drawer-body` / 状态行 / 网格

| 选择器 | 属性 | 值 | 来源 |
| --- | --- | --- | --- |
| `.drawer-body` | flex / overflow-y | 1 / auto | Drawer.css:179-180 |
| `.drawer-body` | padding | **22px 32px 40px** | Drawer.css:181 |
| `.drawer-status` | color / font-size | `rgba(255,255,255,0.5)`→`#ffffff80` / 14px | Drawer.css:185-186 |
| `.drawer-status` | padding / text-align | 34px 0 / center | Drawer.css:187-188 |
| `.drawer-grid` | display | grid | Drawer.css:192 |
| `.drawer-grid` | grid-template-columns | `repeat(auto-fill, minmax(116px, 1fr))` | Drawer.css:193 |
| `.drawer-grid` | gap | 16px | Drawer.css:194 |

#### 应用磁贴 `.drawer-app`

| 属性 | 值 | 来源 |
| --- | --- | --- |
| display / flex-direction / align-items | flex / column / center | Drawer.css:198-200 |
| gap | 9px | Drawer.css:201 |
| padding | **16px 8px 12px** | Drawer.css:202 |
| border-radius | **18px** | Drawer.css:203 |
| background（常态） | `rgba(255,255,255,0.06)` → `#ffffff0f` | Drawer.css:204 |
| border（常态） | `1px solid rgba(255,255,255,0.1)` → `#ffffff1a` | Drawer.css:205 |
| box-shadow（常态） | `0 4px 18px rgba(0,0,0,0.25)` → `0 4px 18px #00000040` | Drawer.css:206 |
| cursor / position | pointer / relative | Drawer.css:207、209 |
| background（hover） | `rgba(255,255,255,0.13)` → `#ffffff21` | Drawer.css:213 |
| transform（hover） | `translateY(-4px)` | Drawer.css:214 |
| box-shadow（hover） | `0 10px 26px rgba(0,0,0,0.4)` → `#00000066` | Drawer.css:215 |

磁贴子元素：

| 选择器 | 属性 | 值 | 来源 |
| --- | --- | --- | --- |
| `.drawer-app img` | width / height | **50px × 50px** | Drawer.css:219-220 |
| `.drawer-app img` | object-fit | contain | Drawer.css:221 |
| `.drawer-fallback` | width / height / border-radius | 50px × 50px / **14px** | Drawer.css:225-227 |
| `.drawer-fallback` | background | `linear-gradient(135deg, #5b7cfa, #8a5cf6)` | Drawer.css:228 |
| `.drawer-fallback` | color / font-size / font-weight | `#fff` / 24px / 600 | Drawer.css:229、233-234 |
| `.drawer-fallback` | display / align / justify | flex / center / center | Drawer.css:230-232 |
| `.drawer-app-name` | font-size / color / font-weight | 13px / `#dfe3e9` / 500 | Drawer.css:238-239、247 |
| `.drawer-app-name` | text-align / line-height / max-width | center / 1.3 / 100% | Drawer.css:240-242 |
| `.drawer-app-name` | 行数截断 | overflow hidden + `-webkit-line-clamp: 2`（最多 2 行） | Drawer.css:243-246 |
| `.drawer-app select`（编辑模式） | position / left / right / bottom | absolute / 6px / 6px / 6px | Drawer.css:251-254 |
| `.drawer-app select` | font-size / background / color | 11px / `rgba(10,11,14,0.92)`→`#0a0b0eeb` / `#e8eaed` | Drawer.css:255-257 |
| `.drawer-app select` | border / radius / padding | `1px solid #5b7cfa` / 8px / 3px 6px | Drawer.css:258-260 |

#### 滚动条（`.drawer-body`）

- `::-webkit-scrollbar { width: 10px }` — Drawer.css:263-265
- `::-webkit-scrollbar-thumb { background: rgba(255,255,255,0.22) → #ffffff38; border-radius: 5px }` — Drawer.css:267-270

### 1.2 任务面板（TaskPanel）

#### 根与背景

| 选择器 | 属性 | 值 | 来源 |
| --- | --- | --- | --- |
| `.tp-root` | position / inset / z-index | fixed / 0 / 20 | TaskPanel.css:3-5 |
| `.tp-root` | pointer-events | none（`.open` 时 auto） | TaskPanel.css:6、10 |
| `.tp-backdrop` | position / inset | absolute / 0 | TaskPanel.css:14-15 |
| `.tp-backdrop`（关闭） | background / backdrop-filter | `rgba(8,10,14,0)`→`#080a0e00` / `blur(0px)` | TaskPanel.css:16-17 |
| `.tp-backdrop`（打开） | background / backdrop-filter | `rgba(8,10,14,0.62)`→`#080a0e9e` / `blur(22px) saturate(1.2)` | TaskPanel.css:22-24（见 §5） |
| `.tp-panel` | position / inset | absolute / 0（全屏） | TaskPanel.css:28-29 |
| `.tp-panel` | display / flex-direction | flex / column | TaskPanel.css:30-31 |
| `.tp-panel` | padding | **46px 56px 26px** | TaskPanel.css:32 |
| `.tp-panel` | gap | **18px** | TaskPanel.css:33 |
| `.tp-panel` | background / border / border-radius | **均未声明**（透明，靠 backdrop 着色） | TaskPanel.css:27-37 |

#### 系统信息卡片 `.tp-stats` / `.tp-stat`

| 选择器 | 属性 | 值 | 来源 |
| --- | --- | --- | --- |
| `.tp-stats` | display / grid-template-columns / gap | grid / `repeat(5, 1fr)` / 16px | TaskPanel.css:46-48 |
| `.tp-stat` | background | `rgba(255,255,255,0.08)` → `#ffffff14` | TaskPanel.css:52 |
| `.tp-stat` | border | `1px solid rgba(255,255,255,0.14)` → `#ffffff24` | TaskPanel.css:53 |
| `.tp-stat` | border-radius | **20px** | TaskPanel.css:54 |
| `.tp-stat` | padding | **16px 20px 14px** | TaskPanel.css:55 |
| `.tp-stat` | backdrop-filter | `blur(14px)` | TaskPanel.css:56-57 |
| `.tp-stat-label` | font-size / color / letter-spacing | 13px / `rgba(255,255,255,0.55)`→`#ffffff8c` / 2px | TaskPanel.css:61-63 |
| `.tp-stat-value` | font-size / font-weight | **30px / 700** | TaskPanel.css:67-68 |
| `.tp-stat-value` | margin-top / line-height | 6px / 1.15 | TaskPanel.css:69、71 |
| `.tp-stat-value` | font-variant-numeric | tabular-nums（等宽数字） | TaskPanel.css:70 |
| `.tp-stat-value` | color | 行内样式，取各卡片 `color`（见 §4.3） | TaskPanel.tsx:131 |
| `.tp-stat-sub` | font-size / color / margin-top | 12px / `rgba(255,255,255,0.45)`→`#ffffff73` / 3px | TaskPanel.css:75-77 |
| `.tp-bar` | height / border-radius | **6px / 3px** | TaskPanel.css:81-82 |
| `.tp-bar` | background / margin-top / overflow | `rgba(255,255,255,0.14)`→`#ffffff24` / 10px / hidden | TaskPanel.css:83-85 |
| `.tp-bar-fill` | height / border-radius | 100% / 3px | TaskPanel.css:89-90 |
| `.tp-bar-fill` | width / background | 行内样式：`min(100, max(0, pct))%` 与该卡片 `color` | TaskPanel.tsx:136 |

#### 区块标题与列表

| 选择器 | 属性 | 值 | 来源 |
| --- | --- | --- | --- |
| `.tp-section` | font-size / color / letter-spacing / margin-top | 15px / `rgba(255,255,255,0.7)`→`#ffffffb3` / 3px / 6px | TaskPanel.css:96-99 |
| `.tp-section i` | font-style / font-size / color / margin-left | normal / 12px / `rgba(255,255,255,0.4)`→`#ffffff66` / 8px | TaskPanel.css:103-106 |
| `.tp-section` 文案 | — | `运行中的应用 ` + `<i>{tasks.length}</i>` | TaskPanel.tsx:143 |
| `.tp-list` | flex / overflow-y / flex-direction | 1 / auto / column | TaskPanel.css:110-113 |
| `.tp-list` | gap / padding-right | **12px** / 6px | TaskPanel.css:114-115 |
| `.tp-empty` | color / font-size / padding / text-align | `rgba(255,255,255,0.45)`→`#ffffff73` / 14px / 40px 0 / center | TaskPanel.css:119-122 |
| `.tp-empty` 文案 | — | `暂无运行中的应用` | TaskPanel.tsx:147 |

#### 任务行 `.tp-task`

| 属性 | 值 | 来源 |
| --- | --- | --- |
| display / align-items / gap | flex / center / 16px | TaskPanel.css:126-128 |
| background（常态） | `rgba(255,255,255,0.1)` → `#ffffff1a` | TaskPanel.css:129 |
| border（常态） | `1px solid rgba(255,255,255,0.14)` → `#ffffff24` | TaskPanel.css:130 |
| border-radius | **18px** | TaskPanel.css:131 |
| padding | **14px 20px** | TaskPanel.css:132 |
| cursor | pointer | TaskPanel.css:133 |
| backdrop-filter | `blur(10px)` | TaskPanel.css:135-136 |
| background（hover） | `rgba(255,255,255,0.18)` → `#ffffff2e` | TaskPanel.css:140 |
| transform（hover） | `translateX(6px)`（右移 6px） | TaskPanel.css:141 |

行内子元素：

| 选择器 | 属性 | 值 | 来源 |
| --- | --- | --- | --- |
| `.tp-task img` | width / height / object-fit | **42px × 42px** / contain | TaskPanel.css:145-147 |
| `.tp-fallback` | width / height / border-radius | 42px × 42px / **12px** | TaskPanel.css:151-153 |
| `.tp-fallback` | background | `linear-gradient(135deg, #5b7cfa, #8a5cf6)` | TaskPanel.css:154 |
| `.tp-fallback` | color / font-size / font-weight | `#fff` / 20px / 600 | TaskPanel.css:155、159-160 |
| `.tp-fallback` | flex-shrink | 0 | TaskPanel.css:161 |
| `.tp-task-text` | flex / min-width | 1 / 0 | TaskPanel.css:165-166 |
| `.tp-task-name` | font-size / font-weight / color | **16px / 600 / `#f2f4f7`** | TaskPanel.css:170-172 |
| `.tp-task-title` | font-size / color | 13px / `rgba(255,255,255,0.5)`→`#ffffff80` | TaskPanel.css:176-177 |
| `.tp-task-title` | 溢出 | white-space nowrap + overflow hidden + text-overflow ellipsis | TaskPanel.css:178-180 |

#### 关闭按钮 `.tp-close`

| 属性 | 值 | 来源 |
| --- | --- | --- |
| width / height | **36px × 36px** | TaskPanel.css:184-185 |
| border-radius | 50%（正圆） | TaskPanel.css:186 |
| border | none | TaskPanel.css:187 |
| background（常态） | `rgba(255,255,255,0.12)` → `#ffffff1f` | TaskPanel.css:188 |
| color（常态） | `rgba(255,255,255,0.8)` → `#ffffffcc` | TaskPanel.css:189 |
| font-size / cursor | 16px / pointer | TaskPanel.css:190-191 |
| flex-shrink | 0 | TaskPanel.css:193 |
| background（hover） | `rgba(244,63,94,0.75)` → `#f43f5ebf` | TaskPanel.css:197 |
| color（hover） | `#fff` | TaskPanel.css:198 |
| 字形 | `✕`，`aria-label="关闭"` | TaskPanel.tsx:167、165 |

#### 底部提示与滚动条

- `.tp-hint`：text-align center、font-size 13px、color `rgba(255,255,255,0.45)`→`#ffffff73`、letter-spacing 2px（TaskPanel.css:202-205）；文案 `按 Win 键切换 · Esc 关闭`（TaskPanel.tsx:173）。
- `.tp-list::-webkit-scrollbar { width: 10px }`（TaskPanel.css:208-210）；`::-webkit-scrollbar-thumb { background: rgba(255,255,255,0.22)→#ffffff38; border-radius: 5px }`（TaskPanel.css:212-215）。

### 1.3 颜色 alpha 十六进制换算说明

源码写法为 `rgba()`；上表中的 8 位十六进制按 `alpha 字节 = round(a × 255)` 换算。该规则与构建产物 `Frontend/dist/assets/index-8WrFANw0.css` 中的写法逐项一致（该文件可见段包含 `#00000059`、`#0a0b0e85`、`#ffffff14`、`#ffffff0f`、`#ffffff47`、`#ffffff12`、`#ffffff21`、`#ffffff24`、`#ffffff6b`、`#ffffff29`、`#ffffff4d`、`#ffffff0a`）。

---

## 2. 动效表（共 18 条）

- 源码中 **不存在 `@keyframes` 或 `animation` 声明**（对 `Frontend/src` 下 3 个 CSS 文件 grep `@keyframes|animation` 无匹配）。
- 未显式声明 `ease` 之外缓动的项，浏览器默认缓动为 `ease`（`cubic-bezier(0.25, 0.1, 0.25, 1)`）。
- 编号 A1–A18 即本文档统计的动效条目总数：**18**。

| 编号 | 选择器 | 属性 | 时长 | 缓动函数 | 起始值 → 结束值 | 触发条件 | 来源 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A1 | `.drawer-backdrop` | background | 0.4s | ease | `rgba(0,0,0,0)` → `rgba(0,0,0,0.35)`（反向同） | `.drawer-root` 添加/移除 `.open` | Drawer.css:17、16、21 |
| A2 | `.drawer-panel` | transform | **0.52s** | **cubic-bezier(0.16, 1, 0.3, 1)** | `translateY(48px) scale(0.985)` → `translateY(0) scale(1)` | open / close | Drawer.css:37、34、45 |
| A3 | `.drawer-panel` | opacity | **0.52s** | ease | 0 → 1（反向 1 → 0） | open / close | Drawer.css:38、35、46 |
| A4 | `.drawer-panel.dragging` | transition（整体） | **none（0s）** | — | 拖拽期间所有过渡失效 | `.dragging` 类添加/移除 | Drawer.css:49-51 |
| A5 | `.drawer-tab` | all | 0.15s | ease（默认） | 背景 `#ffffff12`↔`#ffffff24`（hover）、`#e8eaed`（active）等 | hover / active 类切换 | Drawer.css:98、109、112-116 |
| A6 | `.drawer-btn` | all | 0.15s | ease（默认） | 背景 `#ffffff14`↔`#ffffff29`、边框 ↔`#ffffff4d`、`.on` → `#e8eaed` | hover / `.on` 类切换 | Drawer.css:139、143-144、147-150 |
| A7 | `.drawer-app` | transform | **0.12s** | ease（默认） | `none` → `translateY(-4px)` | hover 进出 | Drawer.css:208、214 |
| A8 | `.drawer-app` | background | **0.15s** | ease（默认） | `#ffffff0f` ↔ `#ffffff21` | hover 进出 | Drawer.css:208、204、213 |
| A9 | `.drawer-app` | box-shadow | **0.15s** | ease（默认） | `0 4px 18px #00000040` ↔ `0 10px 26px #00000066` | hover 进出 | Drawer.css:208、206、215 |
| A10 | `.tp-backdrop` | background | **0.26s** | ease | `rgba(8,10,14,0)` ↔ `rgba(8,10,14,0.62)` | `.tp-root` open / close | TaskPanel.css:18、16、22 |
| A11 | `.tp-backdrop` | backdrop-filter | **0.26s** | ease | `blur(0px)` → `blur(22px) saturate(1.2)` | open / close | TaskPanel.css:18、17、23 |
| A12 | `.tp-panel` | opacity | **0.26s** | ease | 0 → 1 | open / close | TaskPanel.css:36、34、40 |
| A13 | `.tp-panel` | transform | **0.3s** | **cubic-bezier(0.22, 1, 0.36, 1)** | `scale(0.965) translateY(18px)` → `scale(1) translateY(0)` | open / close | TaskPanel.css:36、35、41 |
| A14 | `.tp-bar-fill` | width | **0.6s** | ease | 当前 % → 新 %（行内 width） | 每 2s 数据刷新后数值变化 | TaskPanel.css:91；TaskPanel.tsx:136、52 |
| A15 | `.tp-task` | background | **0.15s** | ease（默认） | `#ffffff1a` ↔ `#ffffff2e` | hover 进出 | TaskPanel.css:134、129、140 |
| A16 | `.tp-task` | transform | **0.12s** | ease（默认） | `none` → `translateX(6px)` | hover 进出 | TaskPanel.css:134、141 |
| A17 | `.tp-close` | background | **0.15s** | ease（默认） | `rgba(255,255,255,0.12)` ↔ `rgba(244,63,94,0.75)`（同时 color → `#fff`） | hover 进出 | TaskPanel.css:192、188、197-198 |
| A18 | `.drawer-head::before` | transform | **静态（无时长）** | — | `translateX(-50%)`（水平居中定位，不随状态变化） | 常驻 | Drawer.css:74 |

补充事实：
- `transition: none`（A4）生效时，拖拽位移为 1:1 无插值；移除 `.dragging` 并清空行内 transform 的同一帧内恢复 A2 的弹簧插值（Drawer.css:50、Drawer.tsx:97-98）。
- 抽屉关闭态下 `.drawer-panel` 的 `backdrop-filter: blur(60px) saturate(1.6)` 仍被声明（Drawer.css:28），仅由 `opacity: 0`（Drawer.css:35）使其不可见。

---

## 3. 交互状态机

### 3.1 顶层 overlay 状态（App.tsx）

状态类型：`type Overlay = 'none' | 'drawer' | 'tasks'`（App.tsx:6），由 `useState` 保存（App.tsx:12）。渲染映射：

- `<Drawer open={overlay === 'drawer'} onClose={() => setOverlay('none')} />`（App.tsx:53）
- `<TaskPanel open={overlay === 'tasks'} onClose={() => setOverlay('none')} />`（App.tsx:54）

三维视口在挂载时以回调接入（App.tsx:18-21）：

```
viewport = new Viewport(container, { onSwipeUp: () => setOverlay('drawer') })
viewport.start()
// 卸载时 viewport.dispose()   App.tsx:23-25
```

### 3.2 抽屉打开 / 关闭触发器清单

**打开（唯一入口）**

| 触发 | 逻辑 | 来源 |
| --- | --- | --- |
| 桌面滚轮上滑 | `Viewport.onWheel`：`deltaY < 0` → `preventDefault()` + `onSwipeUp()` → `setOverlay('drawer')` | Viewport.ts:249-252、App.tsx:19 |

**关闭（5 条路径）**

| 触发 | 逻辑 | 来源 |
| --- | --- | --- |
| backdrop 点击 | `<div className="drawer-backdrop" onClick={onClose} />` | Drawer.tsx:159 |
| Esc | `window` 上 `keydown`（capture=true）：`key === 'Escape'` → `preventDefault()` + `stopPropagation()` + `onClose()` | Drawer.tsx:56-67 |
| 标题栏下拉超过阈值 | `dy > 90` → `requestAnimationFrame(() => onClose())` | Drawer.tsx:99-102 |
| Win 键事件 `kind === 'wind'` | `setOverlay('none')` | App.tsx:41 |
| 应用磁贴启动成功 | `launch()` 成功后调用 `onClose()` | Drawer.tsx:109 |

**事实说明**：`Drawer.tsx` 中 **没有独立的“关闭按钮”元素**——`.drawer-actions` 内只有“编辑/完成”切换按钮（Drawer.tsx:185-187）。头部提示文案为 `标题栏下拉 · Esc 或 Win+D 关闭`（Drawer.tsx:184）。文件顶部注释声称关闭方式还包括“内容滚动到顶后再滚滚轮向下”（Drawer.tsx:20-21），但 grep `Frontend/src` 下所有 `.tsx` 文件无 `wheel` / `onWheel` / `deltaY` 匹配——**抽屉与任务面板组件本身没有任何 wheel 处理器**。

### 3.3 抽屉头部拖拽伪码（源码 Drawer.tsx:70-103、165-168）

事件绑定：`.drawer-panel` 上 `onPointerDown / onPointerMove / onPointerUp / onPointerCancel`（Drawer.tsx:165-168）。

```
状态: dragRef: { pointerId, startY } | null     // Drawer.tsx:34

handlePointerDown(e):                            // Drawer.tsx:70-82
    t = e.target
    if NOT t.closest('.drawer-head'):            return   // 只允许从标题栏发起 (L73)
    if t.closest('button, input, select,
                  .drawer-app, .drawer-tab'):    return   // 排除交互控件 (L74)
    dragRef.current = { pointerId: e.pointerId,
                        startY: e.clientY }               // 记录起始 Y (L75)
    try { e.currentTarget.setPointerCapture(e.pointerId) }
    catch { /* 忽略 */ }                                 // L76-80
    panelRef.current.classList.add('dragging')            // 关闭过渡 A4 (L81)

handlePointerMove(e):                            // Drawer.tsx:84-89
    drag = dragRef.current
    if drag is null
       or drag.pointerId != e.pointerId
       or panelRef.current is null:       return          // L86
    dy = e.clientY - drag.startY                          // 坐标差 (L87)
    if dy > 0:
        panel.style.transform = 'translateY(' + dy + 'px)' // 只跟手下移 (L88)
        // dy <= 0 时不写 transform：上移不产生位移

handlePointerUp(e) / handlePointerCancel(e):     // Drawer.tsx:91-103（同一实现）
    drag = dragRef.current
    if drag is null or drag.pointerId != e.pointerId: return  // L93
    dragRef.current = null                                 // L94
    dy = e.clientY - drag.startY                           // 用局部 drag 计算 (L95)
    panel.classList.remove('dragging')                     // 恢复过渡 A2/A3 (L97)
    panel.style.transform = ''                             // 清行内 transform → 弹簧回位 (L98)
    if dy > 90:                                            // 阈值 90px (L99)
        requestAnimationFrame(() => onClose())              // 延迟一帧再收起 (L101)
```

判定阈值与方向（事实）：
- 方向：仅 **向下**（`dy > 0`）才写位移；`dy <= 0` 不移动面板。
- 关闭阈值：`dy > 90` 像素；`dy <= 90` 仅回弹不关闭。
- 拖拽期间 `touch-action: none`（Drawer.css:41）。

### 3.4 滚轮判定（Viewport.ts，抽屉打开的唯一入口）

监听注册：`renderer.domElement.addEventListener('wheel', onWheel, { passive: false })`（Viewport.ts:129）。

```
onWheel(event):                                  // Viewport.ts:248-257
    if event.deltaY < 0:                         // 方向：上滑（负值）
        event.preventDefault()
        onSwipeUp()                              // → App: setOverlay('drawer')
        return
    if event.deltaY <= 0 OR NOT clickActive:     // 下滑且不在点击态：忽略
        return
    event.preventDefault()
    reverseClick()                               // 下滑仅用于倒放三维动效（Viewport.ts:247 注释）
```

- 阈值：**无幅度阈值**——只要 `deltaY < 0`（任意小数）即触发打开；方向判定仅用符号。
- 下滑（`deltaY > 0`）在 `clickActive === false` 时不做事（Viewport.ts:254）。
- 参考常量：`CLICK_SLOP = 5`（Viewport.ts:24），仅用于三维视口“点击 vs 拖拽旋转”的位移判定（`Math.hypot(dx, dy) > 5` 视为拖拽，Viewport.ts:237），与抽屉拖拽无关。
- 抽屉打开后，`.drawer-root.open` 的 `pointer-events: auto`（Drawer.css:10）覆盖全屏（fixed inset 0，Drawer.css:3-4），而 wheel 监听只挂在 canvas 上（Viewport.ts:129）。

### 3.5 Tab 切换（Drawer.tsx:171-181、146-155）

```
groups = [ {name:'全部', items: apps},
           ...cats.map(c => ({name:c, items: apps.filter(x => x.cat === c)})),
           {name:'未分类', items: apps.filter(x => !x.cat || !cats.includes(x.cat))} ]
        .filter(g => g.name=='全部' || g.name=='未分类' || g.items.length > 0)   // L146-153

shown = groups.find(g => g.name == active) ?? groups[0]                        // L155
active 初始值 = '全部'                                                          // L27

onTabClick(name): setActive(name)                                              // L176
按钮 DOM: <button class={'drawer-tab' + (shown?.name===g.name ? ' active':'')}>
            {g.name}<i>{g.items.length}</i>                                    // L175-179
```

- 每个 tab 显示分类名 + `<i>` 计数徽标（数量 = 该组 `items.length`）。
- 命中 `.active` 的 tab 应用 A5 的反色样式（Drawer.css:112-116）。
- tab 位于 `.drawer-tabs` 内，`overflow-x: auto` 横向可滚（Drawer.css:85）。

### 3.6 应用磁贴点击行为（Drawer.tsx:210-231、105-113）

```
onTileClick(app):                                  // L210: () => !edit && launch(app.id)
    if edit == true: return                        // 编辑模式下点击不启动
    launch(app.id):
        r = await fetch(BRIDGE + '/launch?id=' + encodeURIComponent(id))   // L107
        if NOT r.ok: throw                         // L108
        onClose()                                  // 成功 → 关闭抽屉 (L109)
        catch: setStatus('启动失败：' + id)          // L111 → 渲染于 .drawer-status (L207)
```

编辑模式附加行为：
- `.drawer-app` 内渲染 `<select>`（L218-229），其 `onClick` 调 `e.stopPropagation()`（L220），阻止触发磁贴的 launch。
- `onChange` → `setCat(app.id, value)`（L221）：先本地乐观更新 `setApps`（L117），再 `POST /categories`（L118-123），失败时 `setStatus('分类保存失败')`（L125）。
- `select` 当前值：`cats.includes(app.cat) ? app.cat : '未分类'`（L219）；若 `app.cat` 不在 `cats` 中，额外插入 `<option value="未分类">未分类</option>`（L223）。
- “编辑”按钮切换 `edit`（L185-187），文案 `编辑` ↔ `完成`，开启时按钮带 `.on` 类。
- 新增分类：输入框 `Enter` 或“添加分类”按钮 → `addCategory()`（L197、199）：`name = newCat.trim()`，为空或已存在则直接返回（L130-131），否则本地追加并 `POST /categories-list`（L132-140），失败 `setStatus('新分类保存失败')`（L142）。

### 3.7 任务面板打开 / 关闭

**打开**

| 触发 | 逻辑 | 来源 |
| --- | --- | --- |
| Win 键轮询发现新事件且 `kind !== 'wind'` | `setOverlay(o => o === 'tasks' ? 'none' : 'tasks')`（当前为 `'drawer'` 时会切到 `'tasks'`） | App.tsx:42 |

**关闭（4 条路径）**

| 触发 | 逻辑 | 来源 |
| --- | --- | --- |
| backdrop 点击 | `<div className="tp-backdrop" onClick={onClose} />` | TaskPanel.tsx:125 |
| Esc | `window` `keydown`（capture）：`key === 'Escape'` → preventDefault + stopPropagation + onClose | TaskPanel.tsx:60-71 |
| 任务行点击 | `onClick={() => focusTask(t.id)}` → 请求后无条件 `onClose()` | TaskPanel.tsx:149、73-78 |
| Win 键 `kind === 'wind'` | `setOverlay('none')` | App.tsx:41 |

事实：`TaskPanel` 中同样 **没有独立关闭按钮**——每行的 `.tp-close` 只关闭单个任务，不关闭面板。底部提示文案：`按 Win 键切换 · Esc 关闭`（TaskPanel.tsx:173）。

### 3.8 任务行 hover 与关闭按钮

```
任务行 .tp-task:hover                    // TaskPanel.css:139-142
    background: rgba(255,255,255,0.1) → rgba(255,255,255,0.18)   // A15, 0.15s
    transform : none → translateX(6px)                             // A16, 0.12s

关闭按钮 .tp-close:hover                 // TaskPanel.css:196-199
    background: rgba(255,255,255,0.12) → rgba(244,63,94,0.75)     // A17, 0.15s
    color    : rgba(255,255,255,0.8) → #fff

点击 .tp-close                            // TaskPanel.tsx:159-168
    e.stopPropagation()                    // 不触发任务行 focusTask (L162)
    closeTask(t.id):
        await fetch(BRIDGE + '/close?id=' + encodeURIComponent(id))  // L82
        setTasks(prev => prev.filter(t => t.id !== id))              // L83 移除该行
        catch { }                                                    // L84 网络异常时保持原列表
```

`focusTask(id)`（TaskPanel.tsx:73-78）：`await fetch('/focus?id=' + id)`，`catch {}` 吞掉异常，随后 **无条件** `onClose()`。

### 3.9 轮询节奏

| 轮询 | 间隔 | 时机 | 来源 |
| --- | --- | --- | --- |
| `GET /winkey-state` | **400ms** | 应用启动后持续（`setInterval`） | App.tsx:31-46 |
| `GET /sysinfo` + `GET /tasks` | **2000ms** | 仅任务面板 `open` 期间（首次立即执行） | TaskPanel.tsx:51-52 |
| `GET /apps.json` + `GET /categories` | **一次性** | 首次打开抽屉时（`loadedRef` 防重入） | Drawer.tsx:31、45、51-53 |

`/winkey-state` 的 seq 处理（App.tsx:34-43）：
- 首个响应只记录 `lastSeq`，不动作（L35-38）。
- 之后 `j.seq !== lastSeq` 才处理：`kind === 'wind'` → `setOverlay('none')`（L41）；否则 toggle `'tasks'`（L42）。
- 请求失败 `catch(() => {})` 静默忽略（L45）；未检查 `r.ok`。

---

## 4. 数据来源（HTTP 端点）

### 4.1 端点总清单（共 10 条，全部位于 `http://127.0.0.1:8787`）

| # | 方法 | 路径 | 调用方 | 来源 |
| --- | --- | --- | --- | --- |
| 1 | GET | `/apps.json` | Drawer 首次打开 | Drawer.tsx:38 |
| 2 | GET | `/categories` | Drawer 首次打开 | Drawer.tsx:38 |
| 3 | GET | `/launch?id=<enc>` | 磁贴点击 | Drawer.tsx:107 |
| 4 | POST | `/categories` | 归类 select 变更 | Drawer.tsx:119-123 |
| 5 | POST | `/categories-list` | 添加分类 | Drawer.tsx:136-140 |
| 6 | GET | `/winkey-state` | App 全局轮询（400ms） | App.tsx:32 |
| 7 | GET | `/sysinfo` | TaskPanel 打开时 | TaskPanel.tsx:42 |
| 8 | GET | `/tasks` | TaskPanel 打开时 | TaskPanel.tsx:46 |
| 9 | GET | `/focus?id=<enc>` | 任务行点击 | TaskPanel.tsx:75 |
| 10 | GET | `/close?id=<enc>` | 关闭按钮点击 | TaskPanel.tsx:82 |

`id` 均经 `encodeURIComponent(id)` 编码（Drawer.tsx:107、TaskPanel.tsx:75、82）。

### 4.2 请求体 / 响应 JSON 结构

| 端点 | 请求 | 响应结构（TypeScript 接口，源码为准） | 来源 |
| --- | --- | --- | --- |
| GET `/apps.json` | 无 | `{ apps: AppEntry[] }`；`AppEntry = { id: string, name: string, cat: string, icon: string }`；读取 `a.apps ?? []` | Drawer.tsx:6-11、40、42 |
| GET `/categories` | 无 | `{ cats: string[] }`；读取 `c.cats ?? []` | Drawer.tsx:41、43 |
| GET `/launch` | 无 | 响应体未使用，仅检查 `r.ok` | Drawer.tsx:107-108 |
| POST `/categories` | `Content-Type: application/json`，body = **对象**：`{ [appId]: categoryName, ... }`（由当前全部 app 的 `id → cat` 映射加上本次变更合并，`JSON.stringify(next)`） | 响应体未使用 | Drawer.tsx:116、119-123 |
| POST `/categories-list` | `Content-Type: application/json`，body = **字符串数组** `string[]`（新分类列表，`JSON.stringify(next)`） | 响应体未使用 | Drawer.tsx:132、136-140 |
| GET `/winkey-state` | 无 | `{ seq: number, kind: string }`（`kind` 实际比较值：`'wind'`） | App.tsx:34、41 |
| GET `/sysinfo` | 无 | `SysInfo = { cpu: number|null, mem: { totalGB, availGB, usedPct }|null, net: { rxKbps, txKbps }|null, disk: { totalGB, freeGB, usedPct }|null, gpu: number|null }` | TaskPanel.tsx:6-12、44 |
| GET `/tasks` | 无 | `TaskItem[]`（数组）；`TaskItem = { id: string, name: string, title: string, pid: number, icon: string }`；读取 `Array.isArray(j) ? j : []` | TaskPanel.tsx:14-20、48 |
| GET `/focus` | 无 | 响应完全忽略（含 `r.ok` 也未检查） | TaskPanel.tsx:75-76 |
| GET `/close` | 无 | 响应体未使用；`r.ok` 未检查 | TaskPanel.tsx:82-83 |

请求检查差异（事实）：
- Drawer 的 `load` 检查 `!aRes.ok || !cRes.ok` → `throw`（Drawer.tsx:39）。
- TaskPanel、App 的 `fetch` 均 **不检查 `r.ok`**，只用 `catch` 吞网络错误（TaskPanel.tsx:45、49、76、84；App.tsx:45）。

### 4.3 失败 / 空态兜底展示

| 场景 | 兜底展示 | 渲染位置 | 来源 |
| --- | --- | --- | --- |
| `/apps.json` 或 `/categories` 请求失败或非 2xx | `启动桥未连接（dsh-bridge 未运行）` | `.drawer-status`（color `#ffffff80`、14px、padding 34px 0、居中） | Drawer.tsx:47、207；Drawer.css:184-189 |
| `/launch` 失败 | `启动失败：' + id` | 同上 | Drawer.tsx:111 |
| `POST /categories` 网络失败 | `分类保存失败` | 同上 | Drawer.tsx:125 |
| `POST /categories-list` 网络失败 | `新分类保存失败` | 同上 | Drawer.tsx:142 |
| 当前分类无应用且无状态信息 | `此分类暂无应用` | 同上（条件 `shown.items.length===0 && !status`） | Drawer.tsx:234-236 |
| `/sysinfo` 失败（`sys` 保持 null） | 5 张卡片数值全部显示 `—`；`sub` 为空（GPU 卡因 `sys` 为 null 也不显示“无独立 GPU”）；进度条宽度 0% | `.tp-stat-value` / `.tp-stat-sub` / `.tp-bar-fill` | TaskPanel.tsx:88、92-93、100-101、108-109、116-117；TaskPanel.tsx:136 |
| `/tasks` 失败或返回非数组 | `暂无运行中的应用`（`.tp-empty`） | `.tp-list` 首行 | TaskPanel.tsx:48、147 |
| `/winkey-state` 失败 | 静默忽略，无任何展示 | — | App.tsx:45 |

统计卡片的显示规则（TaskPanel.tsx:87-121）：

| key | label | value 格式 | sub 格式 | 颜色 | bar pct |
| --- | --- | --- | --- | --- | --- |
| cpu | CPU | `cpu + '%'` | 无 | `#5b9dff` | `cpu` |
| mem | 内存 | `usedPct + '%'` | `(totalGB-availGB).toFixed(1) + '/' + totalGB + ' GB'` | `#a78bfa` | `usedPct` |
| net | 网络 | `'↓' + rxKbps + '  ↑' + txKbps` | `'Kbps'` | `#34d399` | **0**（固定） |
| disk | 存储 | `usedPct + '%'` | `'剩 ' + freeGB + ' GB'` | `#fbbf24` | `usedPct` |
| gpu | GPU | `gpu + '%'` | `gpu == null` 时为 `'无独立 GPU'` | `#f472b6` | `gpu` |

- 缺数据时 value 一律 `—`（TaskPanel.tsx:88、92、100、108、116）。
- bar 宽度：`Math.min(100, Math.max(0, pct)) + '%'`（TaskPanel.tsx:136），背景为该卡片 `color`。

---

## 5. backdrop-filter / blur 参数（全部出处）

| 选择器 | backdrop-filter | 背景色 | 何时生效 | 来源 |
| --- | --- | --- | --- | --- |
| `.drawer-panel` | **`blur(60px) saturate(1.6)`**（含 `-webkit-` 前缀同值） | `rgba(10,11,14,0.52)` → `#0a0b0e85` | 常驻声明（关闭态由 opacity 0 隐形） | Drawer.css:27-29 |
| `.drawer-backdrop` | **无 blur** | 关闭 `rgba(0,0,0,0)`；打开 `rgba(0,0,0,0.35)` | 随 `.open` 过渡 | Drawer.css:16、21 |
| `.tp-backdrop` | 关闭 `blur(0px)`；打开 **`blur(22px) saturate(1.2)`**（含 `-webkit-` 同值） | 关闭 `rgba(8,10,14,0)`；打开 `rgba(8,10,14,0.62)` → `#080a0e9e` | 随 `.open` 过渡（A10/A11，0.26s） | TaskPanel.css:16-17、22-24 |
| `.tp-stat` | **`blur(14px)`**（含 `-webkit-` 同值） | `rgba(255,255,255,0.08)` | 常驻 | TaskPanel.css:56-57、52 |
| `.tp-task` | **`blur(10px)`**（含 `-webkit-` 同值） | `rgba(255,255,255,0.1)` | 常驻 | TaskPanel.css:135-136、129 |
| `.tp-panel` | 无 | 无背景 | — | TaskPanel.css:27-37 |

dist 对照：压缩产物对 `.drawer-panel` 输出 `-webkit-backdrop-filter:blur(60px)saturate(1.6)` 与 `background:#0a0b0e85`，与源码一致。

---

## 6. 图标 / 图片资源

### 6.1 磁贴图标（Drawer）

- 有 `app.icon` 时：`<img src={BRIDGE + app.icon} alt="" draggable={false} />`（Drawer.tsx:211-212）→ 实际 URL 形如 `http://127.0.0.1:8787<path>`（`icon` 字段来自 `/apps.json` 的 `AppEntry.icon`，Drawer.tsx:10）。
- 渲染尺寸：**50px × 50px**，`object-fit: contain`（Drawer.css:219-221）。
- 无 `icon`（空字符串/缺失）时：`<div className="drawer-fallback">{app.name.slice(0, 1)}</div>`（Drawer.tsx:213-214）。

### 6.2 磁贴 fallback 渐变色块（Drawer）

`linear-gradient(135deg, #5b7cfa, #8a5cf6)`（Drawer.css:228）——**固定渐变，对所有磁贴相同，不随应用变化**；文字取 `app.name` 的第 1 个字符（Drawer.tsx:214），`color: #fff`、`font-size: 24px`、`font-weight: 600`、居中（Drawer.css:229、233-234、230-232）；块尺寸 50×50、圆角 14px（Drawer.css:225-227）。

### 6.3 任务行图标（TaskPanel）

- 有 `t.icon` 时：`<img src={BRIDGE + t.icon} alt="" draggable={false} />`（TaskPanel.tsx:151）。
- 渲染尺寸：**42px × 42px**，`object-fit: contain`（TaskPanel.css:145-147）。
- 无 `icon` 时：`<div className="tp-fallback">{t.name.slice(0, 1).toUpperCase()}</div>`（TaskPanel.tsx:153）——**取首字符并转大写**（与抽屉不同）。
- fallback 样式：42×42、圆角 12px、同一固定渐变 `linear-gradient(135deg, #5b7cfa, #8a5cf6)`、`color:#fff`、`font-size:20px`、`font-weight:600`、`flex-shrink:0`（TaskPanel.css:151-161）。

### 6.4 其它图片

- 两个组件中除上述 `img` 外无其它图片资源；`Frontend/index.html` 也不引用任何图片。
- 所有 `img` 均设 `draggable={false}` 与 `alt=""`（Drawer.tsx:212、TaskPanel.tsx:151）。

---

## 7. 字体栈与抗锯齿

### 7.1 字体栈（global.css:21-27，唯一一处 font-family 声明）

```css
font-family:
  -apple-system,
  BlinkMacSystemFont,
  'PingFang SC',
  'Microsoft YaHei',
  'Segoe UI',
  sans-serif;
```

- 在 Windows 上实际可命中的字体依次为 **`Microsoft YaHei`（微软雅黑）→ `Segoe UI` → `sans-serif`**（前两项为 macOS/Chromium 前缀名，Windows 不可用）。
- 全仓库 `Frontend/src` 无 `@font-face`（grep 无匹配），不加载任何自定义字体文件；`index.html` 也无字体链接。
- 全局文字色：`#e8eaed`（global.css:28）。
- 各组件未单独声明 `font-family`，全部继承 body 字体栈；组件内只改变 `font-size` / `font-weight` / `color` / `letter-spacing`（已在 §1 逐条列出）。
- 字重使用值汇总：400（默认）、500（`.drawer-app-name`，Drawer.css:247）、600（`.drawer-tab.active`、`.drawer-fallback`、`.tp-fallback`、`.tp-task-name`）、700（`.tp-stat-value`）。
- `.tp-stat-value` 额外声明 `font-variant-numeric: tabular-nums`（TaskPanel.css:70）。

### 7.2 抗锯齿设置（global.css:29-30）

| 属性 | 值 | 来源 |
| --- | --- | --- |
| `-webkit-font-smoothing` | `antialiased` | global.css:29 |
| `-moz-osx-font-smoothing` | `grayscale` | global.css:30 |

- 源码仅含上述两条，未设置 `text-rendering`、`font-kerning`、`font-synthesis` 等其它文本渲染属性（对 `Frontend/src` grep `text-rendering|font-face|font-family|font-smoothing` 仅命中 global.css:21、29、30）。
- 颜色方案：`:root { color-scheme: dark }`（global.css:1-3）+ `<meta name="color-scheme" content="dark">`（index.html:6）。

---

## 8. dist 压缩产物对照（Frontend/dist/assets/index-8WrFANw0.css）

单行压缩文件，与源码一致的可核对项（该文件可读段）：

- `.drawer-root{z-index:10;pointer-events:none;position:fixed;inset:0}`
- `.drawer-backdrop{background:0 0;transition:background .4s}`、`.drawer-root.open .drawer-backdrop{background:#00000059}`
- `.drawer-panel{-webkit-backdrop-filter:blur(60px)saturate(1.6);background:#0a0b0e85;border:1px solid #ffffff14;box-shadow:inset 0 1px #ffffff0f;transition:transform .52s cubic-bezier(.16,1,.3,1),opacity .52s;transform:translateY(48px)scale(.985);opacity:0}`
- `.drawer-panel.dragging{transition:none}`
- `.drawer-head{padding:22px 32px 12px;gap:18px;cursor:grab;border-bottom:1px solid #ffffff14}`
- `.drawer-head:before{background:#ffffff47;border-radius:3px;width:46px;height:5px;top:7px;transform:translate(-50%)}`
- `.drawer-tab{background:#ffffff12;border:1px solid #ffffff21;border-radius:999px;padding:7px 18px;font-size:14px;transition:all .15s}`、hover `#ffffff24`、active `#e8eaed/#101216`
- `.drawer-btn{background:#ffffff14;border:1px solid #ffffff24;border-radius:10px;padding:7px 18px;font-size:13px;transition:all .15s}`、hover `#ffffff29/#ffffff4d`
- `.drawer-hint{color:#ffffff6b;font-size:12px}`
- `.drawer-editbar{background:#ffffff0a;border-bottom:1px solid #ffffff12}`

（压缩文件把 `rgba()` 统一改写为 8 位十六进制，换算规则见 §1.3；未被可读段覆盖的条目以源码为准。）
