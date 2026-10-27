#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CoopTD DPS Meter - Python Desktop Overlay (Detailed Diagnostic Edition)
Connects via ADB to external roxy_reader binary (Zero LSPosed, Zero Injection).
"""

import sys
import os
import json
import subprocess
import threading
import time
import tkinter as tk
from tkinter import ttk, messagebox

ADB_CANDIDATES = [
    r"C:\Users\user\Downloads\platform-tools\adb.exe",
    "adb",
    r"C:\Program Files\Netease\MuMuPlayerGlobal-12.0\shell\adb.exe",
    r"C:\Program Files\Netease\MuMuPlayer-12.0\shell\adb.exe",
    r"D:\Program Files\Netease\MuMuPlayerGlobal-12.0\shell\adb.exe",
    r"C:\leidian\LDPlayer9\adb.exe",
    r"D:\leidian\LDPlayer9\adb.exe",
]

EMULATOR_PORTS = [16384, 16416, 7555, 5555, 62001]

def find_adb():
    for candidate in ADB_CANDIDATES:
        try:
            res = subprocess.run([candidate, "version"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=2)
            if res.returncode == 0:
                return candidate
        except Exception:
            continue
    return "adb"


def format_damage(val: float) -> str:
    """
    Format damage values with 3 decimals (x.xxx) into game tiers:
    None (<1,000), K (1e3), M (1e6), B (1e9), T (1e12), Qa (1e15), Qi (1e18)
    """
    if val < 0:
        return f"-{format_damage(-val)}"
    if val == 0:
        return "0.000"

    tier_units = ["", "K", "M", "B", "T", "Qa", "Qi"]
    tier = 0
    scaled = float(val)

    while scaled >= 1000.0 and tier < len(tier_units) - 1:
        scaled /= 1000.0
        tier += 1

    # Guard against rounding rollover (e.g. 999.9999 -> 1000.000)
    if round(scaled, 3) >= 1000.0 and tier < len(tier_units) - 1:
        scaled /= 1000.0
        tier += 1

    unit = tier_units[tier]
    return f"{scaled:.3f} {unit}".strip()


class DpsMeterApp:
    def __init__(self, root):
        self.root = root
        self.root.title("RoxyDPS - CoopTD Damage Meter (v0.5)")
        self.root.geometry("760x620")
        self.root.minsize(620, 480)
        self.root.configure(bg="#181825")

        self.is_topmost = True
        self.root.wm_attributes("-topmost", self.is_topmost)

        self.adb_path = find_adb()
        self.worker_thread = None
        self.running = False
        self.subproc = None

        self.log_file_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dps_debug.log")
        try:
            with open(self.log_file_path, "a", encoding="utf-8") as f:
                f.write(f"\n{'='*70}\n[RoxyDPS Session Started] {time.strftime('%Y-%m-%d %H:%M:%S')}\n{'='*70}\n")
        except Exception:
            pass

        self.self_total = 0.0
        self.team_total = 0.0
        self.grand_total = 0.0
        self.units_data = []
        self.recent_logs = []

        self.setup_ui()

    def setup_ui(self):
        style = ttk.Style(self.root)
        style.theme_use("clam")

        BG_DARK = "#181825"
        CARD_BG = "#1e1e2e"
        ACCENT = "#89b4fa"
        TEXT_MAIN = "#cdd6f4"
        TEXT_MUTED = "#a6adc8"

        style.configure("Treeview",
                        background="#1e1e2e",
                        foreground="#cdd6f4",
                        fieldbackground="#1e1e2e",
                        rowheight=26,
                        font=("Segoe UI", 9))
        style.configure("Treeview.Heading",
                        background="#313244",
                        foreground="#89b4fa",
                        font=("Segoe UI", 9, "bold"))
        style.map("Treeview", background=[('selected', '#45475a')])

        # 1. Top Control Bar
        ctrl_frame = tk.Frame(self.root, bg=BG_DARK, pady=6, padx=10)
        ctrl_frame.pack(fill=tk.X)

        self.status_lbl = tk.Label(ctrl_frame, text="🔴 Offline", fg="#f38ba8", bg=BG_DARK, font=("Segoe UI", 9, "bold"))
        self.status_lbl.pack(side=tk.LEFT, padx=5)

        self.btn_toggle = tk.Button(ctrl_frame, text="Start Monitor", command=self.toggle_monitor,
                                    bg="#a6e3a1", fg="#11111b", font=("Segoe UI", 9, "bold"), relief=tk.FLAT, padx=12)
        self.btn_toggle.pack(side=tk.LEFT, padx=6)

        self.btn_reset = tk.Button(ctrl_frame, text="Reset Stats", command=self.reset_stats,
                                   bg="#fab387", fg="#11111b", font=("Segoe UI", 9, "bold"), relief=tk.FLAT, padx=8)
        self.btn_reset.pack(side=tk.LEFT, padx=4)

        self.btn_clear_log = tk.Button(ctrl_frame, text="Clear Log", command=self.clear_logs,
                                       bg="#45475a", fg="#cdd6f4", font=("Segoe UI", 8), relief=tk.FLAT, padx=6)
        self.btn_clear_log.pack(side=tk.LEFT, padx=4)

        self.btn_open_log = tk.Button(ctrl_frame, text="Open Log File", command=self.open_log_file,
                                      bg="#313244", fg="#89b4fa", font=("Segoe UI", 8, "bold"), relief=tk.FLAT, padx=6)
        self.btn_open_log.pack(side=tk.LEFT, padx=4)

        self.var_auto_reset = tk.BooleanVar(value=True)
        self.chk_auto_reset = tk.Checkbutton(ctrl_frame, text="Auto-Reset (New Match)", variable=self.var_auto_reset,
                                             bg=BG_DARK, fg=TEXT_MUTED, selectcolor=CARD_BG,
                                             activebackground=BG_DARK, font=("Segoe UI", 9),
                                             command=self.toggle_auto_reset)
        self.chk_auto_reset.pack(side=tk.LEFT, padx=6)

        self.pin_btn = tk.Checkbutton(ctrl_frame, text="Always on Top", bg=BG_DARK, fg=TEXT_MUTED,
                                      selectcolor=CARD_BG, activebackground=BG_DARK,
                                      font=("Segoe UI", 9), command=self.toggle_topmost)
        self.pin_btn.select()
        self.pin_btn.pack(side=tk.RIGHT, padx=5)

        # 2. Team Summary Card
        summary_card = tk.Frame(self.root, bg=CARD_BG, padx=15, pady=8)
        summary_card.pack(fill=tk.X, padx=10, pady=4)

        self.lbl_grand_total = tk.Label(summary_card, text="TOTAL DAMAGE: 0.000",
                                        font=("Segoe UI", 15, "bold"), fg="#cdd6f4", bg=CARD_BG)
        self.lbl_grand_total.pack(anchor=tk.W)

        self.bar_canvas = tk.Canvas(summary_card, height=12, bg="#313244", bd=0, highlightthickness=0)
        self.bar_canvas.pack(fill=tk.X, pady=5)

        sub_frame = tk.Frame(summary_card, bg=CARD_BG)
        sub_frame.pack(fill=tk.X)

        self.lbl_self = tk.Label(sub_frame, text="Self: 0.000 (0.0%)", fg="#a6e3a1", bg=CARD_BG, font=("Segoe UI", 10, "bold"))
        self.lbl_self.pack(side=tk.LEFT)

        self.lbl_team = tk.Label(sub_frame, text="Teammate: 0.000 (0.0%)", fg="#89dceb", bg=CARD_BG, font=("Segoe UI", 10, "bold"))
        self.lbl_team.pack(side=tk.RIGHT)

        # 3. Tower Damage Ranking Table
        table_frame = tk.Frame(self.root, bg=BG_DARK, padx=10, pady=4)
        table_frame.pack(fill=tk.BOTH, expand=True)

        columns = ("tag", "name", "total", "pct", "hits", "crits", "last", "entity")
        self.tree = ttk.Treeview(table_frame, columns=columns, show="headings", selectmode="browse")

        self.tree.heading("tag", text="Player")
        self.tree.heading("name", text="Unit Name")
        self.tree.heading("total", text="Total Dmg")
        self.tree.heading("pct", text="Share %")
        self.tree.heading("hits", text="Hits")
        self.tree.heading("crits", text="Crits")
        self.tree.heading("last", text="Last Dmg")
        self.tree.heading("entity", text="Entity Handle")

        self.tree.column("tag", width=75, anchor=tk.CENTER)
        self.tree.column("name", width=140, anchor=tk.W)
        self.tree.column("total", width=115, anchor=tk.E)
        self.tree.column("pct", width=65, anchor=tk.CENTER)
        self.tree.column("hits", width=55, anchor=tk.CENTER)
        self.tree.column("crits", width=55, anchor=tk.CENTER)
        self.tree.column("last", width=95, anchor=tk.E)
        self.tree.column("entity", width=110, anchor=tk.CENTER)

        scrollbar = ttk.Scrollbar(table_frame, orient=tk.VERTICAL, command=self.tree.yview)
        self.tree.configure(yscroll=scrollbar.set)

        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

        self.tree.tag_configure("self_tag", foreground="#a6e3a1")
        self.tree.tag_configure("team_tag", foreground="#89dceb")

        # 4. Detailed Diagnostic & Combat Log Box
        log_frame = tk.Frame(self.root, bg="#11111b", padx=10, pady=6, height=110)
        log_frame.pack(fill=tk.X, padx=10, pady=5)
        log_frame.pack_propagate(False)

        self.log_text = tk.Text(log_frame, bg="#11111b", fg="#cdd6f4", font=("Consolas", 8),
                                bd=0, highlightthickness=0, state=tk.DISABLED)
        self.log_text.pack(fill=tk.BOTH, expand=True)

    def log(self, msg):
        # 1. Write timestamped log entry to persistent debug log file on disk
        ts = time.strftime("%Y-%m-%d %H:%M:%S")
        try:
            with open(self.log_file_path, "a", encoding="utf-8") as f:
                f.write(f"[{ts}] {msg}\n")
        except Exception:
            pass

        # 2. Update GUI log box (capped at 150 lines to keep UI responsive)
        self.log_text.config(state=tk.NORMAL)
        self.log_text.insert(tk.END, msg + "\n")
        try:
            num_lines = int(self.log_text.index('end-1c').split('.')[0])
            if num_lines > 150:
                self.log_text.delete('1.0', f'{num_lines - 120}.0')
        except Exception:
            pass
        self.log_text.see(tk.END)
        self.log_text.config(state=tk.DISABLED)

    def open_log_file(self):
        try:
            if not os.path.exists(self.log_file_path):
                with open(self.log_file_path, "w", encoding="utf-8") as f:
                    f.write("=== RoxyDPS Debug Log ===\n")
            os.startfile(self.log_file_path)
        except Exception:
            messagebox.showinfo("Log File", f"Debug log saved at:\n{self.log_file_path}")

    def clear_logs(self):
        self.log_text.config(state=tk.NORMAL)
        self.log_text.delete("1.0", tk.END)
        self.log_text.config(state=tk.DISABLED)
        # Record clear event in file
        self.log("🧹 GUI Log view cleared (Full disk log preserved).")

    def toggle_topmost(self):
        self.is_topmost = not self.is_topmost
        self.root.wm_attributes("-topmost", self.is_topmost)

    def toggle_auto_reset(self):
        enabled = 1 if self.var_auto_reset.get() else 0
        if self.subproc and self.subproc.poll() is None:
            try:
                self.subproc.stdin.write(f"auto_reset {enabled}\n")
                self.subproc.stdin.flush()
            except Exception:
                pass
        self.log(f"⚙️ Auto-Reset on new match: {'Enabled' if enabled else 'Disabled (Manual Reset Only)'}")

    def toggle_monitor(self):
        if not self.running:
            self.running = True
            self.btn_toggle.config(text="Stop Monitor", bg="#f38ba8")
            self.status_lbl.config(text="🟡 Connecting...", fg="#f9e2af")
            self.worker_thread = threading.Thread(target=self.reader_worker, daemon=True)
            self.worker_thread.start()
        else:
            self.stop_monitor()

    def stop_monitor(self):
        self.running = False
        if self.subproc:
            try:
                self.subproc.kill()
            except Exception:
                pass
            self.subproc = None
        self.btn_toggle.config(text="Start Monitor", bg="#a6e3a1")
        self.status_lbl.config(text="🔴 Offline", fg="#f38ba8")

    def reset_stats(self):
        self.self_total = 0.0
        self.team_total = 0.0
        self.grand_total = 0.0
        self.units_data.clear()
        self.update_ui_state()
        if self.subproc and self.subproc.poll() is None:
            try:
                self.subproc.stdin.write("reset\n")
                self.subproc.stdin.flush()
            except Exception:
                pass
        self.log("🔄 Statistics reset manually.")

    def update_ratio_bar(self):
        self.bar_canvas.delete("all")
        w = self.bar_canvas.winfo_width()
        h = self.bar_canvas.winfo_height()
        if w <= 10 or self.grand_total <= 0:
            return

        self_pct = self.self_total / self.grand_total
        split_x = int(w * self_pct)

        if split_x > 0:
            self.bar_canvas.create_rectangle(0, 0, split_x, h, fill="#a6e3a1", width=0)
        if split_x < w:
            self.bar_canvas.create_rectangle(split_x, 0, w, h, fill="#89dceb", width=0)

    def update_ui_state(self):
        self.lbl_grand_total.config(text=f"TOTAL DAMAGE: {format_damage(self.grand_total)}")
        self_pct = (self.self_total / self.grand_total * 100.0) if self.grand_total > 0 else 0.0
        team_pct = (self.team_total / self.grand_total * 100.0) if self.grand_total > 0 else 0.0
        self.lbl_self.config(text=f"Self: {format_damage(self.self_total)} ({self_pct:.1f}%)")
        self.lbl_team.config(text=f"Teammate: {format_damage(self.team_total)} ({team_pct:.1f}%)")
        self.update_ratio_bar()

        for item in self.tree.get_children():
            self.tree.delete(item)

        sorted_units = sorted(self.units_data, key=lambda x: x.get("total", 0.0), reverse=True)
        for u in sorted_units:
            tag = u.get("tag", "Self")
            row_tag = "self_tag" if tag == "Self" else "team_tag"
            self.tree.insert("", tk.END, values=(
                f"[{tag}]",
                u.get("name", "Tower"),
                format_damage(u.get("total", 0.0)),
                f"{u.get('pct', 0.0):.1f}%",
                u.get("hits", 0),
                u.get("crits", 0),
                format_damage(u.get("last", 0.0)),
                u.get("entity", "")
            ), tags=(row_tag,))

    def ensure_adb_connected(self):
        try:
            chk = subprocess.run([self.adb_path, "devices"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=3)
            lines = [l for l in chk.stdout.splitlines() if "\tdevice" in l]
            if lines:
                return True
        except Exception:
            pass

        for port in EMULATOR_PORTS:
            addr = f"127.0.0.1:{port}"
            try:
                subprocess.run([self.adb_path, "connect", addr], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=2)
                chk = subprocess.run([self.adb_path, "devices"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=2)
                if "\tdevice" in chk.stdout:
                    return True
            except Exception:
                continue

        return False

    def reader_worker(self):
        bin_dir = os.path.dirname(os.path.abspath(__file__))
        local_bin = os.path.join(bin_dir, "roxy_reader")

        self.root.after(0, lambda: self.log(f"🔎 Using ADB: {self.adb_path}"))

        while self.running:
            if not self.ensure_adb_connected():
                self.root.after(0, lambda: self.status_lbl.config(text="🔴 No Emulator Connected", fg="#f38ba8"))
                self.root.after(0, lambda: self.log("⚠️ Emulator not detected! Trying to connect ports (16384, 7555, 5555)..."))
                time.sleep(2)
                continue

            # Ensure ADB Root
            try:
                subprocess.run([self.adb_path, "root"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3)
                subprocess.run([self.adb_path, "wait-for-device"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3)
                subprocess.run([self.adb_path, "shell", "setenforce 0"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=2)

                id_res = subprocess.run([self.adb_path, "shell", "id"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=3)
                id_str = id_res.stdout.strip()
                self.root.after(0, lambda s=id_str: self.log(f"🔑 ADB Identity: {s}"))
            except Exception as e:
                self.root.after(0, lambda err=str(e): self.log(f"⚠️ ADB root check warning: {err}"))

            # Push roxy_reader
            if os.path.exists(local_bin):
                subprocess.run([self.adb_path, "push", local_bin, "/data/local/tmp/roxy_reader"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                subprocess.run([self.adb_path, "shell", "chmod 777 /data/local/tmp/roxy_reader"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                self.root.after(0, lambda: self.log("📦 Pushed latest roxy_reader to /data/local/tmp/"))
            else:
                self.root.after(0, lambda: self.log(f"❌ Error: {local_bin} not found! Please run build.bat first!"))
                self.root.after(0, self.stop_monitor)
                return

            # Launch
            cmd = [
                self.adb_path, "shell",
                "/data/local/tmp/roxy_reader"
            ]
            try:
                self.subproc = subprocess.Popen(
                    cmd,
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    bufsize=1
                )
                # Sync auto_reset initial preference
                init_auto = 1 if self.var_auto_reset.get() else 0
                if self.subproc.stdin:
                    self.subproc.stdin.write(f"auto_reset {init_auto}\n")
                    self.subproc.stdin.flush()

                self.root.after(0, lambda: self.status_lbl.config(text="🟢 Active (Scanning Process)", fg="#a6e3a1"))
                self.root.after(0, lambda: self.log("🚀 roxy_reader launched. Listening to JSON stream..."))

                # Background thread to capture all stderr output into debug log
                def read_stderr(proc):
                    try:
                        for err_line in proc.stderr:
                            err_line = err_line.strip()
                            if err_line:
                                self.root.after(0, lambda el=err_line: self.log(f"⚠️ [STDERR] {el}"))
                    except Exception:
                        pass

                threading.Thread(target=read_stderr, args=(self.subproc,), daemon=True).start()
            except Exception as e:
                self.root.after(0, lambda: self.status_lbl.config(text=f"🔴 Launch Failed", fg="#f38ba8"))
                self.root.after(0, lambda err=str(e): self.log(f"❌ Launch error: {err}"))
                time.sleep(3)
                continue

            try:
                for line in self.subproc.stdout:
                    if not self.running:
                        break
                    line = line.strip()
                    if not line:
                        continue

                    # If not json, print directly as raw diagnostic
                    if not line.startswith("{"):
                        self.root.after(0, lambda raw=line: self.log(f"📋 {raw}"))
                        continue

                    try:
                        data = json.loads(line)
                        ev_type = data.get("type")

                        if ev_type == "diag":
                            msg = data.get("msg", "")
                            self.root.after(0, lambda m=msg: self.log(f"🔧 [Diag] {m}"))

                        elif ev_type == "status":
                            state = data.get("state")
                            msg = data.get("msg", "")
                            if state == "battle":
                                self.root.after(0, lambda: self.status_lbl.config(text="⚔️ In Battle", fg="#a6e3a1"))
                            elif state == "lobby":
                                self.root.after(0, lambda: self.status_lbl.config(text="🏠 In Lobby", fg="#89dceb"))
                            elif state == "scanning":
                                self.root.after(0, lambda: self.status_lbl.config(text="🔍 Searching Game", fg="#f9e2af"))
                            elif state == "offline":
                                self.root.after(0, lambda: self.status_lbl.config(text="🟡 Waiting for Restart", fg="#f9e2af"))
                            if msg:
                                self.root.after(0, lambda m=msg: self.log(f"ℹ️ {m}"))

                        elif ev_type == "init":
                            pid = data.get("pid")
                            pkg = data.get("pkg")
                            base = data.get("base")
                            elf_ok = data.get("elf_ok")
                            self.root.after(0, lambda: self.status_lbl.config(text=f"🟢 Attached ({pkg} | PID {pid})", fg="#a6e3a1"))
                            self.root.after(0, lambda p=pid, b=base, k=pkg, e=elf_ok: self.log(f"🎯 ATTACHED! Package:{k} | PID:{p} | libil2cpp:{b} (ELF Magic:{'OK' if e else 'BAD'})"))

                        elif ev_type == "reset":
                            self.self_total = 0.0
                            self.team_total = 0.0
                            self.grand_total = 0.0
                            self.units_data.clear()
                            reason = data.get("reason", "new_match")
                            self.root.after(0, lambda r=reason: self.log(f"🔄 Battle Reset ({r}). Ready."))
                            self.root.after(0, self.update_ui_state)

                        elif ev_type == "local_player":
                            idx = data.get("index", -1)
                            self.root.after(0, lambda i=idx: self.log(f"👤 Local Player Index: P{i}"))

                        elif ev_type == "hit":
                            crit_mark = " (CRIT!)" if data.get("crit") else ""
                            dmg = data.get("dmg", 0.0)
                            raw_dmg = data.get("raw_dmg", dmg)
                            dmg_str = format_damage(dmg)
                            p_ref = data.get("player_ref", -1)
                            p_str = f"|P{p_ref}" if p_ref != -1 else ""
                            
                            diag_parts = []
                            meter_raw = data.get("meter_raw", 0)
                            flip_raw = data.get("flip_raw", 0)
                            dmg_type = data.get("dmg_type", 0)
                            if meter_raw != 0 or flip_raw != 0:
                                if meter_raw != 0: diag_parts.append(f"meter:{meter_raw/10000.0:.1f}")
                                if flip_raw != 0: diag_parts.append(f"flip:{flip_raw/10000.0:.1f}")
                            diag_str = f" [{', '.join(diag_parts)}]" if diag_parts else ""

                            overkill_str = ""
                            if raw_dmg > dmg + 0.1:
                                ovk = raw_dmg - dmg
                                overkill_str = f" [Overkill: {format_damage(ovk)}]"
                                
                            log_entry = f"💥 Tick:{data.get('tick')} | [{data.get('tag')}{p_str}] {data.get('unit_name')} -> {dmg_str}{crit_mark}{overkill_str}{diag_str}"
                            self.root.after(0, lambda entry=log_entry: self.log(entry))

                        elif ev_type == "summary":
                            self.self_total = data.get("self_total", 0.0)
                            self.team_total = data.get("team_total", 0.0)
                            self.grand_total = data.get("grand_total", 0.0)
                            self.units_data = data.get("units", [])
                            self.root.after(0, self.update_ui_state)

                    except Exception:
                        continue

            except Exception:
                pass

            if self.subproc:
                try:
                    self.subproc.kill()
                except Exception:
                    pass
                self.subproc = None

            time.sleep(1)


def main():
    root = tk.Tk()
    app = DpsMeterApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
