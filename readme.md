# CoopTD DPS Meter - Reversing Guide & Documentation

---

### 🎯 Core Goal
Develop an external, pure read-only **DPS & Damage Meter** to monitor tower outputs, total damage, and hits without any in-process hooking.
* **100% External Architecture**: **NO LSPosed, NO Zygisk, NO code modifications, NO memory protection changes (PROT_WRITE)**.
* **Separation of Concerns**:
  1. **Android Reader Daemon (`roxy_reader`)**: Standalone root ELF binary running outside the game process, reading memory via kernel `process_vm_readv(pid, ...)`.
  2. **Python Desktop Overlay (`dps_meter.py`)**: Windows GUI written in Python Tkinter, connecting via ADB to display real-time damage rankings, charts, and hits with zero latency.

---

### 💡 100% Exact In-Game Damage Formula
The game's floating text matches the internal fixed-point integer divided by **`10000.0`**:
* **Normal Hit**: `2,050,000 / 10000.0 = 205.0`
* **Critical Hit**: `3,480,000 / 10000.0 = 348.0` (Crit multiplier: ~170%)
* **Formula**: `DisplayDamage = RawDamage / 10000.0`

---

### 🛡️ External Memory Reading Architecture (Zero LSPosed)

```text
+-------------------------------------------------------------+
|                     Windows PC (Host)                       |
|                                                             |
|   +-----------------------------------------------------+   |
|   |             Python GUI (dps_meter.py)               |   |
|   |  - Always-On-Top Floating Overlay (Tkinter)         |   |
|   |  - Real-time Tower Rankings, Self vs Teammate       |   |
|   |  - One-click Start / Stop / Reset                   |   |
|   +--------------------------^--------------------------+   |
|                              | JSON stream over ADB         |
+------------------------------|------------------------------+
                               |
                        [ ADB Tunnel ]
                               |
+------------------------------v------------------------------+
|                Android Emulator (MuMu / LDPlayer)           |
|                                                             |
|   +-----------------------------------------------------+   |
|   |     External Native Reader (/data/local/tmp/...)    |   |
|   |  - Standalone executable (Zero injection)           |   |
|   |  - Pure process_vm_readv(pid, ...)                  |   |
|   |  - 500Hz high-frequency polling (2ms)               |   |
|   |  - Streams JSON events: {"type":"hit", ...}         |   |
|   +--------------------------^--------------------------+   |
|                              | process_vm_readv             |
|                              | (Kernel Read-Only)           |
|   +--------------------------v--------------------------+   |
|   |               Game Process (cooptd)                 |   |
|   |  - 100% Untouched, NO LSPosed, NO Hooks, NO Mod     |   |
|   +-----------------------------------------------------+   |
+-------------------------------------------------------------+
```

---

### 🔍 Technical Solved Issues Summary

1. **Prediction Rollback & Tick Rewind Deduplication**:
   - Photon Quantum performs client prediction rollbacks during normal gameplay, causing simulation ticks to jump backwards (e.g. tick 1250 -> 1220).
   - Any naive reset based on `tick < last_tick` breaks in-game counting.
   - Using deterministic `HitKey { Tick, TowerEntity, EnemyEntity, RawDamage }` eliminates duplicate counting across rollbacks without resetting statistics mid-battle.
2. **Manual Reset & Controlled Auto-Reset**:
   - Python UI provides a **"Reset Stats"** button that sends `reset\n` directly into `roxy_reader` stdin.
   - An **"Auto-Reset (New Match)"** checkbox allows players to choose between match-based automatic reset (default) or multi-game persistent accumulation (manual reset only).
3. **APK Direct Loading & 60MB Code Segment Scanner**:
   - Modern Unity APKs map native libraries directly from `base.apk` without exposing `libil2cpp.so` in `/proc/<pid>/maps`.
   - Multi-layered scanner inspects 30MB~120MB executable (`r-xp`) segments, locates ELF magic `\x7fELF`, and verifies memory structure.
4. **Local Player Dynamic Identification**:
   - Local player can be Player 0 or Player 1 in co-op.
   - Dynamically resolved via `QuantumGame (+0x20) -> Session (+0x10) -> _localPlayerMap (+0x10) -> Players[0]`.
5. **Unboxed Component Memory Offsets**:
   - Quantum ECS `ComponentDataBuffer` stores unboxed structs (no 16-byte object header).
   - Offsets: `UnitId` = `0x14`, `PlayerRef` = `0x20`, `myEntity` = `0x68`.

---

### 🚀 How to Build & Run

#### 1. Compile `roxy_reader`
In `C:\Users\user\Desktop\gemi\20261026`:
```cmd
python build.py
```
*(Or specify custom NDK path: `python build.py C:\path\to\ndk`)*

#### 2. Run Python GUI Overlay
```cmd
python dps_meter.py
```
- Click **"Start Monitor"**: The script automatically connects via ADB, pushes `roxy_reader` to `/data/local/tmp/`, launches it as root, and streams combat data live to the floating overlay!
- Click **"Reset Stats"**: Manually clears statistics at any time.
- Toggle **"Auto-Reset (New Match)"**: Uncheck if you want stats to accumulate continuously across multiple games without auto-clearing.
