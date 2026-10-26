#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>
#include <link.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <map>
#include <unordered_set>
#include <string>
#include <sstream>
#include <iomanip>

#define TAG "RoxyDPS"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

uintptr_t g_il2cpp_base = 0;
int g_local_player_ref = -1; // -1: Unresolved, 0: Player 0, 1: Player 1

// ==========================================
// 📍 Verified Offsets & RVAs
// ==========================================
#define OFFSET_TYPEINFO_SLOT     0xcf4fb10 // libil2cpp.so + 0xcf4fb10
#define OFFSET_STATIC_FIELDS     0xb8      // Il2CppClass -> static_fields
#define EVENT_ID_HIT_BULLET      24        // 0x18: EventOnHitBullet

// ==========================================
// 🛡️ Safe Read Template (Pure Memory Only)
// ==========================================
template <typename T>
bool SafeRead(uintptr_t addr, T& out_val) {
    if (addr < 0x10000) return false;
    out_val = *(reinterpret_cast<T*>(addr));
    return true;
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
// 📊 Hit Record for Perfect Deduplication
// ==========================================
// Photon Quantum rollbacks re-simulate identical ticks with new heap objects.
// Keying by (Tick, TowerEntity, EnemyEntity, RawDamage) ensures zero duplicate counting!
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
    int hit_count = 0;
    int crit_count = 0;
    double last_damage = 0.0;
};

std::map<uint64_t, TowerStats> g_tower_stats;
uintptr_t g_tower_buffer_cached = 0;

// ==========================================
// 🔍 Pure-Read GetDataPointer (1-to-1 ARM64 ASM)
// ==========================================
void* PureReadGetDataPointer(uintptr_t buffer, uint64_t entityRef) {
    if (!buffer) return nullptr;
    uint32_t entity_idx = (uint32_t)(entityRef & 0xFFFFFFFF);

    uintptr_t sparse = 0;
    if (!SafeRead(buffer + 0x38, sparse) || !sparse) return nullptr;

    uint16_t dense_idx = 0;
    if (!SafeRead(sparse + (entity_idx * 2), dense_idx) || dense_idx == 0) return nullptr;

    int block_cap = 0;
    uintptr_t blocks_list = 0;
    if (!SafeRead(buffer + 0x28, block_cap) || block_cap <= 0) return nullptr;
    if (!SafeRead(buffer + 0x18, blocks_list) || !blocks_list) return nullptr;

    int block_idx = (int)dense_idx / block_cap;
    int slot_idx  = (int)dense_idx % block_cap;

    uintptr_t block_ptr = 0;
    if (!SafeRead(blocks_list + (block_idx * 16) + 8, block_ptr) || !block_ptr) return nullptr;

    int stride = 0;
    if (!SafeRead(buffer + 0x0c, stride) || stride <= 0) return nullptr;

    return (void*)(block_ptr + slot_idx * stride);
}

// ==========================================
// 👤 Dynamic Local Player Ref Resolver
// ==========================================
int GetLocalPlayerRef(uintptr_t game) {
    if (!game) return -1;
    uintptr_t session = 0, player_map = 0, players_list = 0, items = 0;
    int size = 0;
    if (!SafeRead(game + 0x20, session) || !session) return -1;
    if (!SafeRead(session + 0x10, player_map) || !player_map) return -1;
    if (!SafeRead(player_map + 0x10, players_list) || !players_list) return -1;
    if (!SafeRead(players_list + 0x18, size) || size <= 0) return -1;
    if (!SafeRead(players_list + 0x10, items) || !items) return -1;
    int local_p = -1;
    if (SafeRead(items + 0x20, local_p)) {
        return local_p;
    }
    return -1;
}

// ==========================================
// 🚀 High-Frequency Read-Only Monitoring Thread
// ==========================================
void dps_meter_thread() {
    while (g_il2cpp_base == 0) {
        dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) {
            if (info->dlpi_name && strstr(info->dlpi_name, "libil2cpp.so")) {
                *(uintptr_t*)data = (uintptr_t)info->dlpi_addr;
                return 1;
            }
            return 0;
        }, &g_il2cpp_base);
        usleep(50000);
    }

    LOGI("⚔️ [RoxyDPS] Located libil2cpp.so at: 0x%lx", (unsigned long)g_il2cpp_base);
    LOGI("🛡️ [RoxyDPS] High-Frequency 2ms Poller Activated (Zero Hooks, Rollback Protected)...");

    std::unordered_set<HitKey, HitKeyHash> seen_hits;
    auto last_summary_time = std::chrono::steady_clock::now();
    int current_tick = 0;

    while (true) {
        // High-frequency polling (2ms sleep = 500Hz) to catch all events before clear
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

        uintptr_t type_info_ptr = 0, type_info = 0, static_fields = 0, registry = 0;
        if (!SafeRead(g_il2cpp_base + OFFSET_TYPEINFO_SLOT, type_info_ptr) || !type_info_ptr) continue;
        if (!SafeRead(type_info_ptr, type_info) || !type_info) continue;
        if (!SafeRead(type_info + OFFSET_STATIC_FIELDS, static_fields) || !static_fields) continue;
        if (!SafeRead(static_fields + 0x0, registry) || !registry) continue;

        uintptr_t activeRunners = 0;
        int runner_count = 0;
        if (!SafeRead(registry + 0x10, activeRunners) || !activeRunners) continue;
        if (!SafeRead(activeRunners + 0x18, runner_count) || runner_count <= 0) continue;

        uintptr_t items = 0, runner = 0, game = 0;
        if (!SafeRead(activeRunners + 0x10, items) || !items) continue;
        if (!SafeRead(items + 0x20, runner) || !runner) continue;
        if (!SafeRead(runner + 0x68, game) || !game) continue;

        // Automatically resolve local player index
        if (g_local_player_ref == -1) {
            int detected_p = GetLocalPlayerRef(game);
            if (detected_p != -1) {
                g_local_player_ref = detected_p;
                LOGI("👤 [RoxyDPS] Local Player Ref Detected: %d", g_local_player_ref);
            }
        }

        uintptr_t frames = 0, target_frame = 0;
        if (SafeRead(game + 0x10, frames) && frames) {
            // Prefer Verified frame, fallback to Predicted frame
            SafeRead(frames + 0x10, target_frame);
            if (!target_frame) SafeRead(frames + 0x18, target_frame);
        }

        uintptr_t context = 0, eventList = 0;
        if (!SafeRead(game + 0x90, context) || !context) continue;
        if (!SafeRead(context + 0xd8, eventList) || !eventList) continue;

        int event_count = 0;
        uintptr_t head = 0;
        if (!SafeRead(eventList + 0x10, event_count) || event_count <= 0) continue;
        if (!SafeRead(eventList + 0x18, head) || !head) continue;

        uintptr_t curr = head;
        bool has_new_damage = false;

        do {
            int tick = 0, event_id = 0;
            if (!SafeRead(curr + 0x28, tick) || !SafeRead(curr + 0x38, event_id)) break;

            // Handle new battle / match restart (Tick resets)
            if (current_tick > 1000 && tick < current_tick - 800) {
                LOGI("🔄 [RoxyDPS] New battle detected (Tick %d -> %d). Resetting statistics.", current_tick, tick);
                g_tower_stats.clear();
                seen_hits.clear();
                g_tower_buffer_cached = 0;
                g_local_player_ref = -1;
                current_tick = tick;
            }

            if (tick > current_tick) {
                current_tick = tick;
            }

            if (event_id == EVENT_ID_HIT_BULLET) {
                uint64_t tower_entity = 0, enemy_entity = 0;
                int64_t raw_damage = 0;

                SafeRead(curr + 0x60, tower_entity);
                SafeRead(curr + 0x50, enemy_entity);
                SafeRead(curr + 0x40, raw_damage);

                // Deduplicate by Deterministic Hit Signature (Tick, Tower, Enemy, Damage)
                // Completely prevents rollback re-simulations from multiplying damage!
                HitKey hit_key = {
                    tick,
                    (uint32_t)(tower_entity & 0xFFFFFFFF),
                    (uint32_t)(enemy_entity & 0xFFFFFFFF),
                    (int32_t)(raw_damage & 0xFFFFFFFF)
                };

                if (seen_hits.find(hit_key) == seen_hits.end()) {
                    seen_hits.insert(hit_key);

                    // Exact In-Game Floating Damage: raw / 10000.0 (Matches in-game floating numbers)
                    double display_damage = (double)raw_damage / 10000.0;
                    bool is_crit = (display_damage > 250.0);

                    auto& stats = g_tower_stats[tower_entity];
                    stats.entity = tower_entity;
                    stats.total_damage += display_damage;
                    stats.last_damage = display_damage;
                    stats.hit_count++;
                    if (is_crit) {
                        stats.crit_count++;
                    }

                    // Resolve Tower UnitId & PlayerRef from unboxed ECS ComponentDataBuffer
                    // Note: Unboxed struct offsets are shifted by -0x10 from boxed Il2Cpp metadata!
                    // Unboxed offsets: UnitId: 0x14, PlayerRef: 0x20, myEntity: 0x68
                    if (stats.unit_id == 0 && target_frame) {
                        // 1. Try cached TowerBuffer
                        if (g_tower_buffer_cached) {
                            void* tower_ptr = PureReadGetDataPointer(g_tower_buffer_cached, tower_entity);
                            if (tower_ptr) {
                                uint64_t my_entity = 0;
                                if (SafeRead((uintptr_t)tower_ptr + 0x68, my_entity) && my_entity == tower_entity) {
                                    SafeRead((uintptr_t)tower_ptr + 0x14, stats.unit_id);
                                    SafeRead((uintptr_t)tower_ptr + 0x20, stats.player_ref);
                                }
                            }
                        }

                        // 2. Scan all buffers if not yet resolved (Tower component stride >= 0x300, ~1000 bytes)
                        if (stats.unit_id == 0) {
                            uintptr_t comp_buffers = 0;
                            int comp_len = 0;
                            if (SafeRead(target_frame + 0xd0, comp_buffers) && comp_buffers &&
                                SafeRead(target_frame + 0xd8, comp_len) && comp_len > 0) {
                                for (int bi = 0; bi < comp_len && bi < 265; bi++) {
                                    uintptr_t buf = comp_buffers + (bi * 0x48);
                                    int stride = 0;
                                    SafeRead(buf + 0x0c, stride);

                                    if (stride >= 0x300) {
                                        void* tower_ptr = PureReadGetDataPointer(buf, tower_entity);
                                        if (tower_ptr) {
                                            uint64_t my_entity = 0;
                                            if (SafeRead((uintptr_t)tower_ptr + 0x68, my_entity) && my_entity == tower_entity) {
                                                g_tower_buffer_cached = buf;
                                                SafeRead((uintptr_t)tower_ptr + 0x14, stats.unit_id);
                                                SafeRead((uintptr_t)tower_ptr + 0x20, stats.player_ref);
                                                LOGI("🎯 [Resolved Tower] 0x%llx -> UnitId: %d (%s) | Player: %d",
                                                     (unsigned long long)tower_entity, stats.unit_id,
                                                     GetUnitName(stats.unit_id), stats.player_ref);
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    const char* unit_name = GetUnitName(stats.unit_id);
                    bool is_self = (g_local_player_ref != -1) ? (stats.player_ref == g_local_player_ref) : (stats.player_ref == 0);
                    const char* player_tag = is_self ? "Self" : "Teammate";

                    LOGI("💥 [HIT] Tick:%d | [%s] %s (0x%llx) | Dmg:%.1f%s",
                         tick, player_tag, unit_name,
                         (unsigned long long)tower_entity, display_damage,
                         (is_crit ? " (CRIT!)" : ""));

                    has_new_damage = true;
                }
            }

            uintptr_t next = 0;
            if (!SafeRead(curr + 0x18, next)) break;
            curr = next;
        } while (curr != 0 && curr != head);

        // Prune old seen hits every 500 entries (keep past 600 ticks = ~20 seconds of rollback safety)
        if (seen_hits.size() > 500) {
            for (auto it = seen_hits.begin(); it != seen_hits.end(); ) {
                if (it->tick < current_tick - 600) {
                    it = seen_hits.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Print Summary Ranking Table every 3 seconds
        auto now = std::chrono::steady_clock::now();
        if (has_new_damage && std::chrono::duration_cast<std::chrono::seconds>(now - last_summary_time).count() >= 3) {
            last_summary_time = now;

            double self_total = 0.0, teammate_total = 0.0;
            for (const auto& pair : g_tower_stats) {
                bool is_self = (g_local_player_ref != -1) ? (pair.second.player_ref == g_local_player_ref) : (pair.second.player_ref == 0);
                if (is_self) self_total += pair.second.total_damage;
                else teammate_total += pair.second.total_damage;
            }
            double grand_total = self_total + teammate_total;

            LOGI("==================== 🏆 TOWER DAMAGE RANKING 🏆 ====================");
            for (const auto& pair : g_tower_stats) {
                const auto& s = pair.second;
                const char* unit_name = GetUnitName(s.unit_id);
                bool is_self = (g_local_player_ref != -1) ? (s.player_ref == g_local_player_ref) : (s.player_ref == 0);
                const char* player_tag = is_self ? "Self" : "Teammate";
                double pct = (grand_total > 0) ? (s.total_damage / grand_total * 100.0) : 0.0;

                LOGI("  [%s] %s (ID:%d | 0x%llx) -> Total: %.1f (%.1f%%) | Hits: %d | Crits: %d | Last: %.1f",
                     player_tag, unit_name, s.unit_id,
                     (unsigned long long)s.entity, s.total_damage, pct,
                     s.hit_count, s.crit_count, s.last_damage);
            }
            LOGI("-------------------------------------------------------------------");
            if (grand_total > 0) {
                LOGI("  📢 TEAM SUMMARY | Self: %.1f (%.1f%%) | Teammate: %.1f (%.1f%%)",
                     self_total, (self_total / grand_total * 100.0),
                     teammate_total, (teammate_total / grand_total * 100.0));
            }
            LOGI("  📢 GRAND TOTAL TEAM DAMAGE: %.1f", grand_total);
            LOGI("===================================================================");
        }
    }
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    std::thread(dps_meter_thread).detach();
    return JNI_VERSION_1_6;
}
