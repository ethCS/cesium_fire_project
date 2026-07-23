# Fire Simulation 2 — Wildfire Visualization in Unreal Engine 5

A real-time wildfire visualization project built on **Unreal Engine 5.7** using **Cesium for Unreal** for georeferenced 3D terrain. Fire perimeter data is sourced from the USFS MTBS (Monitoring Trends in Burn Severity) API and streamed into the engine at runtime.

---

## Prerequisites

| Tool | Version | Notes |
|---|---|---|
| Unreal Engine | 5.7 | Install via Epic Games Launcher |
| Visual Studio | 2022 | Workloads: **Game development with C++** and **.NET desktop development** |
| Python | 3.9+ | Required for the data pipeline only |
| Cesium ion account | — | Free at [cesium.com/ion](https://cesium.com/ion) |

---

## Setup

### 1. Install the Cesium for Unreal Plugin

1. Open the **Epic Games Launcher → Unreal Engine → Marketplace**
2. Search for **Cesium for Unreal** and click **Free / Install to Engine**
3. Select engine version **5.7**

> The `GeoReferencing` and `ModelingToolsEditorMode` plugins are built into UE5 — no separate install needed.

### 2. Clone and Open the Project

```bash
git clone https://github.com/ethCS/cesium_fire_project.git
```

Right-click `firesimulation2.uproject` → **Generate Visual Studio project files**, then open `firesimulation2.sln` in Visual Studio and build (**Development Editor | Win64**).

Alternatively, double-click `firesimulation2.uproject` — UE will prompt to compile on first open.

### 3. Add Your Cesium ion Token

1. Open the project in UE5
2. In the **Cesium panel** (Window → Cesium), paste your ion access token
3. The token is saved locally in `Content/CesiumSettings/` which is git-ignored — it will not be committed

### 4. Install Required Packaged Assets (Content + FireData)

Because `Content.zip` is larger than GitHub's 2GB git-pack limit, project assets are distributed through a GitHub Release instead of normal git history.

```powershell
.\Tools\install_project_assets.ps1
```

This installs:
- `Content/` (maps, materials, Niagara assets, etc.)
- `FireData/` (prebuilt generated data)

If you only need one archive:

```powershell
# Content only
.\Tools\install_project_assets.ps1 -SkipFireData

# FireData only
.\Tools\install_project_assets.ps1 -SkipContent
```

### 5. Set Up / Regenerate Fire Data (optional)

The `FireData/Generated/` folder holds runtime data consumed by the engine. It is **not tracked in git** — you must generate it before running.

#### Option A — Fetch directly from the USFS API (recommended, no downloads needed)

```bash
cd Tools
pip install -r requirements-fire-pipeline.txt

# Fetch a specific year
python fetch_mtbs_api.py --years 2024 --output-dir ../FireData/Generated

# Fetch multiple years
python fetch_mtbs_api.py --years 2022,2023,2024 --output-dir ../FireData/Generated

# Fetch all available years (~1984–present, slow)
python fetch_mtbs_api.py --all --output-dir ../FireData/Generated
```

#### Option B — Process a local MTBS shapefile ZIP

```powershell
# Download a ZIP from https://mtbs.gov/direct-download
.\Tools\run_fire_pipeline.ps1 -InputZipFolder "C:\path\to\mtbs_zip_folder" -OutputFolder ".\FireData\Generated"
```

After either option you should have:
```
FireData/Generated/
  catalog/events.ndjson
  index/year_state_day_index.json
  geometry/<year>.ndjson  (one file per year fetched)
```

### 6. Configure Data Paths (if needed)

Data paths and coordinate origin are set in **Project Settings → Project → Fire Simulation Data**.

| Setting | Default |
|---|---|
| Generated Data Root | `FireData/Generated` |
| Origin Latitude | `39.8283` (geographic center of the US) |
| Origin Longitude | `-98.5795` |
| Meters Per Unreal Unit | `1.0` |

If fires appear in the wrong location on the globe, check these transform values.

---

## Content

The `Content/` folder is **not tracked in git** (binary `.uasset`/`.umap` files). This includes the main level (`FireMain`), Niagara fire effects, and material assets.

Install packaged assets directly from the project release:

`https://github.com/ethCS/cesium_fire_project/releases/tag/project-assets-v1`

---

## Project Structure

```
firesimulation2/
├── Config/                  UE project configuration
├── FireData/
│   └── Generated/           Runtime fire data (git-ignored, generate with Tools/)
├── Source/
│   └── firesimulation2/
│       ├── FireData/        Data loading subsystem and types
│       └── FireVisualization/ Rendering components, HUD, game mode
├── Tools/
│   ├── fetch_mtbs_api.py    Fetch fire data from USFS API
│   ├── fire_data_etl.py     Process local MTBS shapefile ZIPs
│   ├── run_fire_pipeline.ps1 PowerShell wrapper for the ETL
│   └── requirements-fire-pipeline.txt
└── firesimulation2.uproject
```
