# UMGAutoBuilder

[English README](./README.md)

`UMGAutoBuilder` 是一个基于 JSON 配置的 UMG（`WidgetBlueprint`）构建、增量更新和导出工具，面向 Unreal Editor 工作流。

这个插件尽量保持**独立、可移植**，不依赖具体游戏运行时模块。通常只要把 `Plugins/UMGAutoBuilder/` 拷到另一个 UE 项目里并重新编译，就可以继续使用。

## 适用场景

`UMGAutoBuilder` 最适合做“**结构化 UI 骨架生成器**”：

- 从 JSON 生成面板型 UMG 结构
- 把已有 WidgetTree 导出成可机器编辑的 JSON
- 对已有控件做保守式 Patch，而不是整棵树重建
- 把 AI 生成的布局描述落地成可继续手工编辑的 UMG 资产

它**不适合**作为所有复杂 `WidgetBlueprint` 的永久唯一真源。更推荐的工作流是：

1. 先生成一个可用的结构骨架
2. 验证控件命名和层级
3. 再交给 UI / 蓝图继续在编辑器里精修

## 功能概览

- **Build 模式**：创建或完全重建目标 `WidgetBlueprint`
- **Patch 模式**：仅更新指定控件属性，或按需追加子控件
- **Export 模式**：把现有 WidgetTree 导出为 JSON
- **createIfMissing**：目标资产不存在时自动创建
- **assertWidgets**：校验关键控件名是否存在
- **编辑器工作台**：通过 `Tools -> UMG Auto Builder` 直接执行 Build / Patch / Export

## 运行要求

- Unreal Engine `5.3+`
- 仅支持编辑器构建目标
- 下方命令示例默认使用 `PowerShell`

## 安装方式

1. 把插件放到项目目录 `Plugins/UMGAutoBuilder/`
2. 视情况重新生成工程文件
3. 编译编辑器目标
4. 在 Unreal Editor 中启用插件
5. 重启编辑器

## 快速开始

### 命令行执行 Build / Patch

```powershell
UnrealEditor-Cmd.exe "<YourProject>.uproject" `
  -run=UMGAutoBuild `
  -Config="C:/Path/To/WidgetSpec.json" `
  -NoAssetRegistryCache -unattended -nop4 -nosplash
```

可选参数：

- `-Mode=build|patch`：覆盖 JSON 中的模式设置

### 导出已有 Widget

```powershell
UnrealEditor-Cmd.exe "<YourProject>.uproject" `
  -run=UMGAutoExport `
  -Widget="/Game/UI/WB_Example.WB_Example" `
  -Out="C:/tmp/WB_Example.json" `
  -Pretty `
  -NoAssetRegistryCache -unattended -nop4 -nosplash
```

参数说明：

- `-Widget=`：要导出的 WidgetBlueprint 资产路径
- `-Out=`：输出 JSON 文件路径
- `-Pretty`：是否格式化导出的 JSON

## 编辑器工作台

启用插件后，可在以下菜单打开：

- `Tools -> UMG Auto Builder`

工作台提供：

- `Config Path`：供 Build / Patch 使用
- `Widget Path` 与 `Export Output Path`：供 Export 使用
- 内联结果面板：显示 warning 和 error

## 当前支持的控件类型

当前 JSON 节点支持的常用控件包括：

- `CanvasPanel`
- `Overlay`
- `VerticalBox`
- `HorizontalBox`
- `WrapBox`
- `SizeBox`
- `Border`
- `Image`
- `TextBlock`
- `ProgressBar`
- `Button`
- `Slider`
- `SpinBox`
- `CheckBox`
- `ComboBoxString`
- `ScrollBox`
- `Spacer`
- `UserWidget`，通过 `props.class` 指定

因此它特别适合状态面板、信息面板、动态列表、数据展示面板等偏“结构化”的 UI 骨架生成场景。

## JSON 结构概览

### 顶层示例

```json
{
  "schemaVersion": 1,
  "targetWidget": "/Game/UI/WB_Example.WB_Example",
  "createIfMissing": true,
  "mode": "build",
  "assertWidgets": ["RootCanvas", "TitleText"],
  "root": {
    "type": "CanvasPanel",
    "name": "RootCanvas",
    "children": []
  },
  "patch": {
    "setWidgetProps": [
      {
        "name": "TitleText",
        "props": { "text": "Hello" }
      }
    ],
    "ensureChildren": [
      {
        "parent": "SomeWrapBox",
        "children": [
          {
            "type": "TextBlock",
            "name": "NewChild",
            "props": { "text": "X" }
          }
        ]
      }
    ]
  }
}
```

### 顶层字段

- `schemaVersion`：当前 schema 版本，目前为 `1`
- `targetWidget`：目标 WidgetBlueprint 的对象路径
- `createIfMissing`：目标资产不存在时是否自动创建
- `mode`：`build` 或 `patch`
- `assertWidgets`：应用完成后必须存在的控件名
- `root`：Build 模式使用的根节点
- `patch`：Patch 模式使用的增量操作集

### 节点字段

- `type`：控件类型
- `name`：控件名称
- `props`：控件属性
- `slot`：父容器上的 Slot 属性
- `children`：子节点数组

### `UserWidget` 额外属性

- `class`：软类路径，例如 `"/Game/UI/WB_Chip.WB_Chip_C"`

## Patch 模型

Patch 模式刻意保持保守：

- `patch.setWidgetProps`：按名称更新已有控件属性
- `patch.ensureChildren`：仅在缺失时追加子节点

这样可以降低对已经手工调整过的 WidgetTree 的破坏风险。

## 推荐工作流

1. 从导出的 JSON 开始，或者手写一份 JSON
2. 用 `build` 先生成第一版 UI 骨架
3. 在结构快速变化阶段继续迭代 JSON
4. 稳定后改用 `patch` 做小步更新
5. 最终把复杂动画、绑定、交互和细节修饰放回 UMG 编辑器中完成

## 说明与限制

- Build 模式会创建一棵新的 WidgetTree，以规避 UE 在重复生成时的命名和类冲突
- Patch 模式不会尝试做深度语义合并
- 当前 Export / Build 主要覆盖常见布局、插槽和展示属性
- 未识别的顶层字段、节点字段或 patch 字段会以 warning 形式输出，便于排查 JSON 规格问题
