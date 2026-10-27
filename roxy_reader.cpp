#include <sys/uio.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <map>
#include <unordered_set>
#include <string>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <vector>
#include <atomic>
#include <algorithm>

// ==========================================
// 📍 Verified Offsets & RVAs
// ==========================================
#define OFFSET_TYPEINFO_SLOT     0xcf4fb10 // libil2cpp.so + 0xcf4fb10
#define OFFSET_STATIC_FIELDS     0xb8      // Il2CppClass -> static_fields
#define EVENT_ID_HIT_BULLET      24        // 0x18: EventOnHitBullet

// Global reset flags triggered by stdin or lobby
std::atomic<bool> g_manual_reset_requested(false);
std::atomic<bool> g_auto_reset_enabled(true);

// ==========================================
// 🛡️ External Process Memory Safe Read (process_vm_readv)
// ==========================================
bool SafeReadRaw(pid_t pid, uintptr_t addr, void* out_val, size_t size) {
    if (addr < 0x10000) return false;
    struct iovec local = { out_val, size };
    struct iovec remote = { reinterpret_cast<void*>(addr), size };
    return process_vm_readv(pid, &local, 1, &remote, 1, 0) == (ssize_t)size;
}

template <typename T>
bool SafeRead(pid_t pid, uintptr_t addr, T& out_val) {
    return SafeReadRaw(pid, addr, &out_val, sizeof(T));
}

// ==========================================
// 📖 Complete Unit Name Resolver (UnitStat.csv)
// ==========================================
const char* GetUnitName(int unitId) {
    switch (unitId) {
        // Golem
        case 10000011: return "Golem_Lv1_A";
        case 10000012: return "Golem_Lv1_B";
        case 10000021: return "Golem_Lv2_A";
        case 10000022: return "Golem_Lv2_B";
        case 10000031: return "Golem_Lv3_A";
        case 10000032: return "Golem_Lv3_B";
        case 10000041: return "Golem_Lv4_A";
        case 10000042: return "Golem_Lv4_B";

        // Skeleton
        case 20000011: return "Skeleton_Lv1_A";
        case 20000012: return "Skeleton_Lv1_B";
        case 20000021: return "Skeleton_Lv2_A";
        case 20000022: return "Skeleton_Lv2_B";
        case 20000031: return "Skeleton_Lv3_A";
        case 20000032: return "Skeleton_Lv3_B";
        case 20000041: return "Skeleton_Lv4_A";
        case 20000042: return "Skeleton_Lv4_B";

        // Orc
        case 30000011: return "Orc_Lv1_A";
        case 30000012: return "Orc_Lv1_B";
        case 30000021: return "Orc_Lv2_A";
        case 30000022: return "Orc_Lv2_B";
        case 30000031: return "Orc_Lv3_A";
        case 30000032: return "Orc_Lv3_B";
        case 30000041: return "Orc_Lv4_A";
        case 30000042: return "Orc_Lv4_B";

        // Dragon
        case 40000011: return "Dragon_Lv1_A";
        case 40000021: return "Dragon_Lv2_A";
        case 40000031: return "Dragon_Lv3_A";
        case 40000041: return "Dragon_Lv4_A";

        // God / Mythic Units
        case 70000001: return "GuardianOfNature";
        case 70000002: return "Destroyer";
        case 70000003: return "GoldCrow";
        case 70000004: return "Maniac";
        case 70000005: return "Draksar";
        case 70000006: return "Regik";
        case 70000007: return "Basilisk";
        case 70000008: return "Oni";
        case 70000009: return "GoblinBiker";
        case 70000010: return "PhantomSlime";
        case 70000011: return "IceKnight";
        case 70000012: return "AncientBlueDragon";
        case 70000013: return "BigHand";
        case 70000014: return "SpaceSoldier";
        case 70000015: return "CoinMan";
        case 70000016: return "MasterCat";
        case 70000017: return "WitchDoctor";
        case 70000018: return "SurpriseMouse";
        case 70000019: return "Helicopter";
        case 70000020: return "Mimic";
        case 70000021: return "LeekDuck";
        case 70000022: return "Clara";
        case 70000023: return "Omen";
        case 70000024: return "Asura";
        case 70000025: return "Cannon";
        case 70000026: return "Eggby";
        case 70000027: return "Eggby2";
        case 70000028: return "Eggby3";
        case 70000029: return "Eggby4";
        case 70000030: return "Ivy";
        case 70000031: return "Whisper";
        case 70000032: return "Kaiser";
        default: return "Tower";
    }
}

// ==========================================
// 📊 Hit Record for Rollback Deduplication
// ==========================================
struct HitKey {
    int32_t tick;
    uint32_t tower_idx;
    uint32_t enemy_idx;
    int32_t damage_raw;

    bool operator==(const HitKey& o) const {
        return tick == o.tick &&
               tower_idx == o.tower_idx &&
               enemy_idx == o.enemy_idx &&
               damage_raw == o.damage_raw;
    }
};

struct HitKeyHash {
    size_t operator()(const HitKey& k) const {
        size_t h = (size_t)k.tick;
        h = h * 31 + (size_t)k.tower_idx;
        h = h * 31 + (size_t)k.enemy_idx;
        h = h * 31 + (size_t)k.damage_raw;
        return h;
    }
};

// ==========================================
// 📊 Tower Damage Statistics
// ==========================================
struct TowerStats {
    uint64_t entity = 0;
    int unit_id = 0;
    int player_ref = 0;
    double total_damage = 0.0;
    double total_raw_damage = 0.0;
    int hit_count = 0;
    int crit_count = 0;
    double last_damage = 0.0;
};

// ==========================================
// 🔍 Pure-Read GetDataPointer (External Memory)
// ==========================================
uintptr_t PureReadGetDataPointer(pid_t pid, uintptr_t buffer, uint64_t entityRef) {
    if (!buffer) return 0;
    uint32_t entity_idx = (uint32_t)(entityRef & 0xFFFFFFFF);

    uintptr_t sparse = 0;
    if (!SafeRead(pid, buffer + 0x38, sparse) || !sparse) return 0;

    uint16_t dense_idx = 0;
    if (!SafeRead(pid, sparse + (entity_idx * 2), dense_idx) || dense_idx == 0) return 0;

    int block_cap = 0;
    uintptr_t blocks_list = 0;
    if (!SafeRead(pid, buffer + 0x28, block_cap) || block_cap <= 0) return 0;
    if (!SafeRead(pid, buffer + 0x18, blocks_list) || !blocks_list) return 0;

    int block_idx = (int)dense_idx / block_cap;
    int slot_idx  = (int)dense_idx % block_cap;

    uintptr_t block_ptr = 0;
    if (!SafeRead(pid, blocks_list + (block_idx * 16) + 8, block_ptr) || !block_ptr) return 0;

    int stride = 0;
    if (!SafeRead(pid, buffer + 0x0c, stride) || stride <= 0) return 0;

    return block_ptr + (slot_idx * stride);
}

// ==========================================
// 👤 Dynamic Local Player Ref Resolver
// ==========================================
int GetLocalPlayerRef(pid_t pid, uintptr_t game) {
    if (!game) return -1;
    uintptr_t session = 0, player_map = 0, players_list = 0, items = 0;
    int size = 0;
    if (!SafeRead(pid, game + 0x20, session) || !session) return -1;
    if (!SafeRead(pid, session + 0x10, player_map) || !player_map) return -1;
    if (!SafeRead(pid, player_map + 0x10, players_list) || !players_list) return -1;
    if (!SafeRead(pid, players_list + 0x18, size) || size <= 0) return -1;
    if (!SafeRead(pid, players_list + 0x10, items) || !items) return -1;
    int local_p = -1;
    if (SafeRead(pid, items + 0x20, local_p)) {
        return local_p;
    }
    return -1;
}

// ==========================================
// 🔬 Verify Candidate Base via ELF Magic + TypeInfo Slot
// ==========================================
bool VerifyIl2CppBase(pid_t pid, uintptr_t base, size_t size_bytes, const std::string& path) {
    // 1. Must be in 64-bit user address space (rejects 32-bit emulator/Houdini stubs like 0xd244000)
    if (base < 0x700000000000ULL) return false;

    // 2. Path MUST belong to the game cooptd (or explicitly be libil2cpp)
    if (path.find("cooptd") == std::string::npos && path.find("libil2cpp") == std::string::npos) {
        return false;
    }

    // 3. Reject dalvik-cache, fonts, resources, or dex files
    if (path.find("dalvik-cache") != std::string::npos ||
        path.find(".dex") != std::string::npos ||
        path.find(".dat") != std::string::npos) {
        return false;
    }

    // 4. Check ELF magic '\x7fELF' (0x464c457f)
    uint32_t magic = 0;
    if (!SafeRead(pid, base, magic) || magic != 0x464c457f) {
        return false;
    }

    // 5. Check 64-bit ELF class (e_ident[EI_CLASS] == 2)
    uint8_t elf_class = 0;
    if (!SafeRead(pid, base + 4, elf_class) || elf_class != 2) {
        return false;
    }

    // 6. Size check: libil2cpp segment is > 50MB (libunity is only 24MB)
    size_t size_mb = size_bytes / (1024 * 1024);
    if (size_mb < 50) {
        return false;
    }

    // 7. Check QuantumRunnerRegistry TypeInfo slot pointer
    uintptr_t type_info_ptr = 0;
    if (SafeRead(pid, base + OFFSET_TYPEINFO_SLOT, type_info_ptr)) {
        if (type_info_ptr != 0 && type_info_ptr < 0x700000000000ULL) {
            return false;
        }
    }

    return true;
}

long GetProcessVmRSS(pid_t pid) {
    char status_path[64];
    snprintf(status_path, sizeof(status_path), "/proc/%d/status", pid);
    FILE* fp = fopen(status_path, "r");
    if (!fp) return 0;
    char line[128];
    long rss = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &rss);
            break;
        }
    }
    fclose(fp);
    return rss;
}

// ==========================================
// 🔍 Advanced Process & Memory Scanner (cooptd Targeting)
// ==========================================
pid_t FindGameProcess(uintptr_t& out_il2cpp_base, std::string& out_pkg_name, std::string& out_diag) {
    DIR* dir = opendir("/proc");
    if (!dir) {
        out_diag = "Cannot opendir /proc (Check root privileges)";
        return -1;
    }

    int scanned = 0;
    int perm_denied = 0;
    std::vector<pid_t> priority_pids;
    std::vector<pid_t> normal_pids;
    std::map<pid_t, std::string> pid_cmdlines;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        int pid = atoi(entry->d_name);
        if (pid <= 0) continue;
        scanned++;

        char cmdline_path[64];
        snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%d/cmdline", pid);
        FILE* fcmd = fopen(cmdline_path, "r");
        char cmdline[256] = {0};
        if (fcmd) {
            fgets(cmdline, sizeof(cmdline), fcmd);
            fclose(fcmd);
        }

        std::string cmd(cmdline);

        // Also check /proc/<pid>/comm if cmdline was empty
        if (cmd.empty()) {
            char comm_path[64];
            snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
            FILE* fcomm = fopen(comm_path, "r");
            char comm[64] = {0};
            if (fcomm) {
                if (fgets(comm, sizeof(comm), fcomm)) {
                    char* nl = strchr(comm, '\n');
                    if (nl) *nl = '\0';
                    cmd = comm;
                }
                fclose(fcomm);
            }
        }

        std::string lower_cmd = cmd;
        for (char& c : lower_cmd) c = tolower(c);

        // Strict exclusion filter: Never inspect emulator hypervisor, bridge, luckydefense, or system daemons
        if (lower_cmd.find("nemu") != std::string::npos ||
            lower_cmd.find("netease") != std::string::npos ||
            lower_cmd.find("mumu") != std::string::npos ||
            lower_cmd.find("vbox") != std::string::npos ||
            lower_cmd.find("qemu") != std::string::npos ||
            lower_cmd.find("goldfish") != std::string::npos ||
            lower_cmd.find("zygote") != std::string::npos ||
            lower_cmd.find("system_server") != std::string::npos ||
            lower_cmd.find("com.android.") != std::string::npos ||
            lower_cmd.find("com.google.") != std::string::npos ||
            lower_cmd.find("roxy_reader") != std::string::npos ||
            lower_cmd.find("adbd") != std::string::npos ||
            lower_cmd.find("logcat") != std::string::npos ||
            lower_cmd.find("luckydefense") != std::string::npos || // Strictly exclude luckydefense
            lower_cmd.empty()) {
            continue;
        }

        // Priority targeting: cooptd has #1 highest priority
        if (lower_cmd.find("cooptd") != std::string::npos) {
            priority_pids.insert(priority_pids.begin(), pid);
        } else if (lower_cmd.find("percent") != std::string::npos) {
            priority_pids.push_back(pid);
        } else {
            normal_pids.push_back(pid);
        }
        pid_cmdlines[pid] = cmd;
    }
    closedir(dir);

    // If target game process is detected, STRICTLY only search the game!
    std::vector<pid_t> candidate_pids;
    if (!priority_pids.empty()) {
        candidate_pids = priority_pids;
    } else {
        candidate_pids = normal_pids;
    }

    if (candidate_pids.empty()) {
        std::stringstream ss;
        ss << "Waiting for cooptd to launch (Scanned " << scanned << ", Blocked " << perm_denied << ")";
        out_diag = ss.str();
        return -1;
    }

    // Sort candidate PIDs by VmRSS descending: the main Unity game process
    // has the largest memory footprint (~1.5GB), whereas forked child stubs/crashpads have <250MB.
    std::sort(candidate_pids.begin(), candidate_pids.end(), [](pid_t a, pid_t b) {
        return GetProcessVmRSS(a) > GetProcessVmRSS(b);
    });

    for (pid_t pid : candidate_pids) {
        char maps_path[64];
        snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
        FILE* fp = fopen(maps_path, "r");
        if (!fp) {
            perm_denied++;
            continue;
        }

        char line[512];
        struct MapEntry {
            uintptr_t start;
            uintptr_t end;
            char perms[8];
            std::string path;
        };
        std::vector<MapEntry> entries;

        while (fgets(line, sizeof(line), fp)) {
            uintptr_t start = 0, end = 0;
            char perms[16] = {0};
            char path[256] = {0};
            int n = sscanf(line, "%lx-%lx %s %*s %*s %*s %s", &start, &end, perms, path);
            if (n >= 3) {
                entries.push_back({start, end, {perms[0], perms[1], perms[2], perms[3], '\0'}, (n >= 4) ? path : ""});
            }
        }
        fclose(fp);

        // Scan memory mappings for libil2cpp.so base (in 64-bit high address space)
        for (const auto& m : entries) {
            if (m.start >= 0x700000000000ULL && m.perms[0] == 'r') {
                if (VerifyIl2CppBase(pid, m.start, m.end - m.start, m.path)) {
                    out_il2cpp_base = m.start;
                    out_pkg_name = pid_cmdlines[pid].empty() ? "com.percent.aos.cooptd" : pid_cmdlines[pid];
                    return pid;
                }
            }
        }
    }

    std::stringstream ss;
    ss << "Inspected " << candidate_pids.size() << " apps (Scanned " << scanned << ", Blocked " << perm_denied << ")";
    out_diag = ss.str();
    return -1;
}

// ==========================================
// 🚀 Main Entry
// ==========================================
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Stdin listener for manual reset commands from Python UI
    std::thread([]() {
        std::string cmd;
        while (std::cin >> cmd) {
            if (cmd == "reset") {
                g_manual_reset_requested.store(true);
            } else if (cmd == "auto_reset") {
                int val = 1;
                if (std::cin >> val) {
                    g_auto_reset_enabled.store(val != 0);
                }
            }
        }
    }).detach();

    uid_t uid = getuid();
    std::cout << "{\"type\":\"diag\",\"msg\":\"RoxyReader started. UID=" << uid << " (eUID=" << geteuid() << ")\"}" << std::endl;
    if (uid != 0) {
        std::cout << "{\"type\":\"diag\",\"msg\":\"⚠️ WARNING: Not running as UID 0! /proc/*/maps access may fail!\"}" << std::endl;
    }

    pid_t game_pid = -1;
    uintptr_t il2cpp_base = 0;
    std::string pkg_name = "";

    std::map<uint64_t, TowerStats> tower_stats;
    std::unordered_set<HitKey, HitKeyHash> seen_hits;
    uintptr_t tower_buffer_cached = 0;
    int local_player_ref = -1;
    int max_tick_seen = 0;
    bool in_battle = false;
    bool has_seen_meter_dmg = false;
    int consecutive_read_failures = 0;
    auto last_summary_time = std::chrono::steady_clock::now();
    auto last_diag_time = std::chrono::steady_clock::now();

    while (true) {
        // ----------------------------------------------------
        // 0. Manual Reset Check (Command from Python UI)
        // ----------------------------------------------------
        if (g_manual_reset_requested.exchange(false)) {
            tower_stats.clear();
            seen_hits.clear();
            tower_buffer_cached = 0;
            max_tick_seen = 0;
            has_seen_meter_dmg = false;
            std::cout << "{\"type\":\"reset\",\"reason\":\"manual\"}" << std::endl;
        }

        // ----------------------------------------------------
        // 1. Process Liveness & Auto-Reconnect
        // ----------------------------------------------------
        if (game_pid == -1 || kill(game_pid, 0) != 0 || consecutive_read_failures > 200) {
            if (game_pid != -1) {
                std::cout << "{\"type\":\"status\",\"state\":\"offline\",\"msg\":\"Game process closed. Waiting for restart...\"}" << std::endl;
            }

            tower_stats.clear();
            seen_hits.clear();
            tower_buffer_cached = 0;
            local_player_ref = -1;
            max_tick_seen = 0;
            in_battle = false;
            has_seen_meter_dmg = false;
            consecutive_read_failures = 0;
            game_pid = -1;
            il2cpp_base = 0;

            while (game_pid == -1 || il2cpp_base == 0) {
                std::string diag;
                game_pid = FindGameProcess(il2cpp_base, pkg_name, diag);

                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - last_diag_time).count() >= 2) {
                    last_diag_time = now;
                    std::cout << "{\"type\":\"status\",\"state\":\"scanning\",\"msg\":\"Searching 60MB Code / il2cpp... " << diag << "\"}" << std::endl;
                }

                if (game_pid == -1) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                }
            }

            std::stringstream ss;
            ss << "{\"type\":\"init\",\"pid\":" << game_pid 
               << ",\"pkg\":\"" << pkg_name << "\""
               << ",\"base\":\"0x" << std::hex << il2cpp_base << std::dec << "\""
               << ",\"elf_ok\":true}";
            std::cout << ss.str() << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));

        // ----------------------------------------------------
        // 2. Quantum Engine Pointers
        // ----------------------------------------------------
        uintptr_t type_info_ptr = 0, type_info = 0, static_fields = 0, registry = 0;
        // Verify memory page readability
        if (!SafeRead(game_pid, il2cpp_base + OFFSET_TYPEINFO_SLOT, type_info_ptr)) {
            consecutive_read_failures++;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        // Memory read was successful! Reset failure counter
        consecutive_read_failures = 0;

        // If TypeInfo slot is NULL (game is loading or at title/lobby), wait calmly without jumping
        if (!type_info_ptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        if (!SafeRead(game_pid, type_info_ptr, type_info) || !type_info) continue;
        if (!SafeRead(game_pid, type_info + OFFSET_STATIC_FIELDS, static_fields) || !static_fields) continue;
        if (!SafeRead(game_pid, static_fields + 0x0, registry) || !registry) continue;

        uintptr_t activeRunners = 0;
        int runner_count = 0;
        if (!SafeRead(game_pid, registry + 0x10, activeRunners) || !activeRunners) continue;
        SafeRead(game_pid, activeRunners + 0x18, runner_count);

        // When in Lobby (between matches), runner_count is 0!
        if (runner_count <= 0) {
            if (in_battle) {
                in_battle = false;
                std::cout << "{\"type\":\"status\",\"state\":\"lobby\",\"msg\":\"Battle finished. In Lobby.\"}" << std::endl;
                tower_buffer_cached = 0;
                if (g_auto_reset_enabled.load()) {
                    local_player_ref = -1;
                    max_tick_seen = 0;
                    has_seen_meter_dmg = false;
                    seen_hits.clear();
                }
            }
            continue;
        }

        uintptr_t items = 0, runner = 0, game = 0;
        if (!SafeRead(game_pid, activeRunners + 0x10, items) || !items) continue;
        if (!SafeRead(game_pid, items + 0x20, runner) || !runner) continue;
        if (!SafeRead(game_pid, runner + 0x68, game) || !game) continue;

        // Transition from Lobby -> New Battle!
        if (!in_battle) {
            in_battle = true;
            if (g_auto_reset_enabled.load()) {
                tower_stats.clear();
                seen_hits.clear();
                tower_buffer_cached = 0;
                local_player_ref = -1;
                max_tick_seen = 0;
                has_seen_meter_dmg = false;
                std::cout << "{\"type\":\"reset\",\"reason\":\"new_match\"}" << std::endl;
            }
            std::cout << "{\"type\":\"status\",\"state\":\"battle\",\"msg\":\"Battle active!\"}" << std::endl;
        }

        // Auto-detect Local Player Index for this match
        if (local_player_ref == -1) {
            int detected_p = GetLocalPlayerRef(game_pid, game);
            if (detected_p != -1) {
                local_player_ref = detected_p;
                std::cout << "{\"type\":\"local_player\",\"index\":" << local_player_ref << "}" << std::endl;
            }
        }

        uintptr_t frames = 0, target_frame = 0;
        if (SafeRead(game_pid, game + 0x10, frames) && frames) {
            SafeRead(game_pid, frames + 0x10, target_frame);
            if (!target_frame) SafeRead(game_pid, frames + 0x18, target_frame);
        }

        uintptr_t context = 0, eventList = 0;
        if (!SafeRead(game_pid, game + 0x90, context) || !context) continue;
        if (!SafeRead(game_pid, context + 0xd8, eventList) || !eventList) continue;

        int event_count = 0;
        uintptr_t head = 0;
        if (!SafeRead(game_pid, eventList + 0x10, event_count) || event_count <= 0) continue;
        if (!SafeRead(game_pid, eventList + 0x18, head) || !head) continue;

        uintptr_t curr = head;
        bool has_new_damage = false;

        do {
            int tick = 0, event_id = 0;
            if (!SafeRead(game_pid, curr + 0x28, tick) || !SafeRead(game_pid, curr + 0x38, event_id)) break;

            if (tick > max_tick_seen) {
                max_tick_seen = tick;
            }

            if (event_id == EVENT_ID_HIT_BULLET) {
                uint64_t tower_entity = 0, enemy_entity = 0;
                int64_t raw_damage = 0;
                int32_t scaling_value = 0;
                int32_t damage_type_id = 0;
                int64_t raw_flip_damage = 0;
                int64_t raw_meter_damage = 0;

                SafeRead(game_pid, curr + 0x60, tower_entity);
                SafeRead(game_pid, curr + 0x50, enemy_entity);
                SafeRead(game_pid, curr + 0x40, raw_damage);
                SafeRead(game_pid, curr + 0x48, scaling_value);
                SafeRead(game_pid, curr + 0x68, damage_type_id);
                SafeRead(game_pid, curr + 0x80, raw_flip_damage);
                SafeRead(game_pid, curr + 0x88, raw_meter_damage);

                // Deterministic hit signature deduplication:
                // When tick rewinds during normal simulation rollback, already processed hits
                // match this key and are safely ignored without needing any reset!
                HitKey hit_key = {
                    tick,
                    (uint32_t)(tower_entity & 0xFFFFFFFF),
                    (uint32_t)(enemy_entity & 0xFFFFFFFF),
                    (int32_t)(raw_damage & 0xFFFFFFFF)
                };

                if (seen_hits.find(hit_key) == seen_hits.end()) {
                    seen_hits.insert(hit_key);

                    double scale = (scaling_value > 0) ? (double)scaling_value : 1.0;
                    double raw_dmg = ((double)raw_damage / 10000.0) * scale;
                    double meter_dmg = ((double)raw_meter_damage / 10000.0) * scale;
                    if (raw_meter_damage > 0) has_seen_meter_dmg = true;

                    // Effective settlement damage: use meter_dmg if active (excludes overkill), else raw_dmg
                    double display_damage = has_seen_meter_dmg ? meter_dmg : raw_dmg;
                    bool is_crit = (damage_type_id == 1);

                    auto& stats = tower_stats[tower_entity];
                    stats.entity = tower_entity;
                    stats.total_damage += display_damage;
                    stats.total_raw_damage += raw_dmg;
                    stats.last_damage = display_damage;
                    stats.hit_count++;
                    if (is_crit) stats.crit_count++;

                    // Resolve Tower metadata (Unboxed offsets: UnitId: 0x14, PlayerRef: 0x20, myEntity: 0x68)
                    if (stats.unit_id == 0 && target_frame) {
                        if (tower_buffer_cached) {
                            uintptr_t tower_ptr = PureReadGetDataPointer(game_pid, tower_buffer_cached, tower_entity);
                            if (tower_ptr) {
                                uint64_t my_entity = 0;
                                if (SafeRead(game_pid, tower_ptr + 0x68, my_entity) && my_entity == tower_entity) {
                                    SafeRead(game_pid, tower_ptr + 0x14, stats.unit_id);
                                    SafeRead(game_pid, tower_ptr + 0x20, stats.player_ref);
                                }
                            }
                        }

                        if (stats.unit_id == 0) {
                            uintptr_t comp_buffers = 0;
                            int comp_len = 0;
                            if (SafeRead(game_pid, target_frame + 0xd0, comp_buffers) && comp_buffers &&
                                SafeRead(game_pid, target_frame + 0xd8, comp_len) && comp_len > 0) {
                                for (int bi = 0; bi < comp_len && bi < 265; bi++) {
                                    uintptr_t buf = comp_buffers + (bi * 0x48);
                                    int stride = 0;
                                    SafeRead(game_pid, buf + 0x0c, stride);

                                    if (stride >= 0x300) {
                                        uintptr_t tower_ptr = PureReadGetDataPointer(game_pid, buf, tower_entity);
                                        if (tower_ptr) {
                                            uint64_t my_entity = 0;
                                            if (SafeRead(game_pid, tower_ptr + 0x68, my_entity) && my_entity == tower_entity) {
                                                tower_buffer_cached = buf;
                                                SafeRead(game_pid, tower_ptr + 0x14, stats.unit_id);
                                                SafeRead(game_pid, tower_ptr + 0x20, stats.player_ref);
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    const char* unit_name = GetUnitName(stats.unit_id);
                    bool is_self = (local_player_ref != -1) ? (stats.player_ref == local_player_ref) : (stats.player_ref == 0);
                    const char* tag = is_self ? "Self" : "Teammate";

                    std::cout << "{\"type\":\"hit\",\"tick\":" << tick 
                              << ",\"entity\":\"0x" << std::hex << tower_entity << std::dec << "\""
                              << ",\"unit_id\":" << stats.unit_id 
                              << ",\"unit_name\":\"" << unit_name << "\""
                              << ",\"tag\":\"" << tag << "\""
                              << ",\"player_ref\":" << stats.player_ref
                              << ",\"dmg\":" << std::fixed << std::setprecision(1) << display_damage 
                              << ",\"raw_dmg\":" << std::fixed << std::setprecision(1) << raw_dmg
                              << ",\"scale\":" << scaling_value
                              << ",\"flip_raw\":" << raw_flip_damage
                              << ",\"meter_raw\":" << raw_meter_damage
                              << ",\"dmg_type\":" << damage_type_id
                              << ",\"crit\":" << (is_crit ? "true" : "false") << "}" << std::endl;

                    has_new_damage = true;
                }
            }

            uintptr_t next = 0;
            if (!SafeRead(game_pid, curr + 0x18, next)) break;
            curr = next;
        } while (curr != 0 && curr != head);

        // Safe pruning: Only prune hits older than 1200 ticks (~40 seconds)
        if (seen_hits.size() > 1000) {
            for (auto it = seen_hits.begin(); it != seen_hits.end(); ) {
                if (it->tick < max_tick_seen - 1200) {
                    it = seen_hits.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Periodic Summary output every 1 second
        auto now = std::chrono::steady_clock::now();
        if (has_new_damage && std::chrono::duration_cast<std::chrono::milliseconds>(now - last_summary_time).count() >= 1000) {
            last_summary_time = now;

            double self_total = 0.0, teammate_total = 0.0;
            for (const auto& pair : tower_stats) {
                bool is_self = (local_player_ref != -1) ? (pair.second.player_ref == local_player_ref) : (pair.second.player_ref == 0);
                if (is_self) self_total += pair.second.total_damage;
                else teammate_total += pair.second.total_damage;
            }
            double grand_total = self_total + teammate_total;

            std::stringstream ss;
            ss << "{\"type\":\"summary\","
               << "\"self_total\":" << std::fixed << std::setprecision(1) << self_total << ","
               << "\"team_total\":" << teammate_total << ","
               << "\"grand_total\":" << grand_total << ","
               << "\"units\":[";

            bool first = true;
            for (const auto& pair : tower_stats) {
                if (!first) ss << ",";
                first = false;
                const auto& s = pair.second;
                bool is_self = (local_player_ref != -1) ? (s.player_ref == local_player_ref) : (s.player_ref == 0);
                double pct = (grand_total > 0) ? (s.total_damage / grand_total * 100.0) : 0.0;

                ss << "{\"entity\":\"0x" << std::hex << s.entity << std::dec << "\","
                   << "\"unit_id\":" << s.unit_id << ","
                   << "\"player_ref\":" << s.player_ref << ","
                   << "\"name\":\"" << GetUnitName(s.unit_id) << "\","
                   << "\"tag\":\"" << (is_self ? "Self" : "Teammate") << "\","
                   << "\"total\":" << s.total_damage << ","
                   << "\"raw_total\":" << s.total_raw_damage << ","
                   << "\"pct\":" << pct << ","
                   << "\"hits\":" << s.hit_count << ","
                   << "\"crits\":" << s.crit_count << ","
                   << "\"last\":" << s.last_damage << "}";
            }
            ss << "]}";
            std::cout << ss.str() << std::endl;
        }
    }

    return 0;
}
