# UMGAutoBuilder

[中文说明](./README.zh-CN.md)

Config-driven UMG (`WidgetBlueprint`) scaffold build, patch, and export tools for Unreal Engine editor workflows.

`UMGAutoBuilder` is designed to stay **portable and project-agnostic**. The plugin does not depend on game-specific runtime modules, so you can copy `Plugins/UMGAutoBuilder/` into another UE project and rebuild it there.

## What It Is For

`UMGAutoBuilder` works best as a **structured widget scaffold generator**:

- build panel-oriented widget trees from JSON
- export an existing widget tree into machine-editable JSON
- patch named widgets without rebuilding the whole asset
- use AI-generated layout descriptions as an editor-friendly starting point

It is **not** meant to replace every handcrafted `WidgetBlueprint` forever. The intended workflow is:

1. generate a usable widget skeleton
2. validate structure and names
3. let UI / Blueprint authors continue polishing in the editor

## Features

- **Build mode**: create or fully rebuild a target `WidgetBlueprint`
- **Patch mode**: update widget props and append children conservatively
- **Export mode**: serialize an existing widget tree into JSON
- **Create-if-missing**: optionally create the target widget asset automatically
- **Assertions**: verify critical widget names with `assertWidgets`
- **Editor workbench**: run Build / Patch / Export inside `Tools -> UMG Auto Builder`

## Requirements

- Unreal Engine `5.3+`
- Editor build only
- Windows-oriented examples in this README use `PowerShell`

## Installation

1. Copy the plugin into your project under `Plugins/UMGAutoBuilder/`
2. Regenerate project files if needed
3. Build the editor target
4. Enable the plugin in Unreal Editor
5. Restart the editor

## Quick Start

### Build or Patch from command line

```powershell
UnrealEditor-Cmd.exe "<YourProject>.uproject" `
  -run=UMGAutoBuild `
  -Config="C:/Path/To/WidgetSpec.json" `
  -NoAssetRegistryCache -unattended -nop4 -nosplash
```

Optional:

- `-Mode=build|patch` overrides the JSON mode

### Export an existing widget

```powershell
UnrealEditor-Cmd.exe "<YourProject>.uproject" `
  -run=UMGAutoExport `
  -Widget="/Game/UI/WB_Example.WB_Example" `
  -Out="C:/tmp/WB_Example.json" `
  -Pretty `
  -NoAssetRegistryCache -unattended -nop4 -nosplash
```

Parameters:

- `-Widget=` widget blueprint asset path
- `-Out=` output JSON path
- `-Pretty` pretty-print exported JSON

## Editor Workbench

After enabling the plugin, open:

- `Tools -> UMG Auto Builder`

The workbench exposes:

- `Config Path` for Build / Patch
- `Widget Path` and `Export Output Path` for Export
- inline result output for warnings and errors

## Supported Widget Types

Current JSON node support covers common layout and display widgets:

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
- `UserWidget` via `props.class`

This makes the plugin especially useful for status panels, info panels, generated lists, and other UI skeletons that benefit from structured authoring.

## JSON Schema Overview

### Top-level example

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

### Top-level fields

- `schemaVersion`: current schema version, currently `1`
- `targetWidget`: target widget blueprint object path
- `createIfMissing`: create the asset if it does not exist
- `mode`: `build` or `patch`
- `assertWidgets`: widget names that must exist after apply
- `root`: root widget tree node used by build mode
- `patch`: incremental operations used by patch mode

### Node fields

- `type`: widget type
- `name`: widget name
- `props`: widget-specific properties
- `slot`: parent slot properties
- `children`: child nodes for panel widgets

### `UserWidget` props

- `class`: soft class path such as `"/Game/UI/WB_Chip.WB_Chip_C"`

## Patch Model

Patch mode intentionally stays conservative:

- `patch.setWidgetProps` updates named existing widgets
- `patch.ensureChildren` appends child nodes only when missing

This keeps generated updates safer for widgets that already have manual adjustments.

## Typical Workflow

1. Start from an exported JSON file or author one manually
2. Run `build` to create a first-pass widget skeleton
3. Iterate in JSON while structure is still changing quickly
4. Switch to `patch` for safer incremental updates
5. Finalize complex behavior, animations, bindings, and polish in UMG

## Notes and Limitations

- Build mode replaces the widget tree with a fresh tree to avoid UE naming / class collisions
- Patch mode does not try to perform deep semantic merges
- Export / Build currently focus on common panel, slot, and display properties
- Unknown top-level, node, or patch fields are reported as warnings to help diagnose malformed specs

