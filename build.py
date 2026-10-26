#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import os
import sys
import shutil
import subprocess

def find_ndk():
    # Allow manual override via command line: python build.py C:\path\to\ndk
    if len(sys.argv) > 1 and os.path.exists(sys.argv[1]):
        p = sys.argv[1]
        for name in ["ndk-build.cmd", "ndk-build"]:
            cand = os.path.join(p, name)
            if os.path.exists(cand):
                return cand
        return p

    # 1. Environment variables
    for env_var in ["ANDROID_NDK_HOME", "ANDROID_NDK", "NDK_ROOT"]:
        p = os.environ.get(env_var)
        if p:
            for name in ["ndk-build.cmd", "ndk-build"]:
                cand = os.path.join(p, name)
                if os.path.exists(cand):
                    return cand

    # 2. Check LOCALAPPDATA\Android\Sdk\ndk
    local_app_data = os.environ.get("LOCALAPPDATA", "")
    if local_app_data:
        ndk_base = os.path.join(local_app_data, "Android", "Sdk", "ndk")
        if os.path.exists(ndk_base):
            try:
                versions = sorted(os.listdir(ndk_base), reverse=True)
                for v in versions:
                    cand = os.path.join(ndk_base, v, "ndk-build.cmd")
                    if os.path.exists(cand):
                        return cand
            except Exception:
                pass

    # 3. Check ANDROID_HOME or ANDROID_SDK_ROOT
    for env_var in ["ANDROID_HOME", "ANDROID_SDK_ROOT"]:
        sdk = os.environ.get(env_var)
        if sdk:
            ndk_base = os.path.join(sdk, "ndk")
            if os.path.exists(ndk_base):
                try:
                    versions = sorted(os.listdir(ndk_base), reverse=True)
                    for v in versions:
                        cand = os.path.join(ndk_base, v, "ndk-build.cmd")
                        if os.path.exists(cand):
                            return cand
                except Exception:
                    pass

    # 4. Check common drives (C:\, D:\)
    for drive in ["C:\\", "D:\\"]:
        for folder in ["Android\\ndk", "Android\\Sdk\\ndk", "android-ndk"]:
            cand_base = os.path.join(drive, folder)
            if os.path.exists(cand_base):
                try:
                    for item in os.listdir(cand_base):
                        cand = os.path.join(cand_base, item, "ndk-build.cmd")
                        if os.path.exists(cand):
                            return cand
                    cand = os.path.join(cand_base, "ndk-build.cmd")
                    if os.path.exists(cand):
                        return cand
                except Exception:
                    pass

    # 5. Check PATH
    which_ndk = shutil.which("ndk-build.cmd") or shutil.which("ndk-build")
    if which_ndk:
        return which_ndk

    return None


def main():
    print("====================================================")
    print("  CoopTD RoxyReader Builder (Python Edition)")
    print("====================================================")

    ndk_cmd = find_ndk()
    if not ndk_cmd:
        print("\n[ERROR] Android NDK was not automatically found on your PC!")
        print("\nHow to solve:")
        print("1. If using Android Studio:")
        print("   - Open Android Studio -> Tools -> SDK Manager -> SDK Tools tab")
        print("   - Check 'NDK (Side by side)' and click Apply to install.")
        print("2. If you already have NDK extracted somewhere, run:")
        print("   python build.py C:\\path\\to\\your\\ndk_folder\n")
        sys.exit(1)

    print(f"[OK] Found NDK: {ndk_cmd}")
    cur_dir = os.path.dirname(os.path.abspath(__file__))
    android_mk = os.path.join(cur_dir, "jni", "Android.mk")
    app_mk = os.path.join(cur_dir, "jni", "Application.mk")

    print("[INFO] Building roxy_reader...")
    build_args = [
        ndk_cmd,
        f"NDK_PROJECT_PATH={cur_dir}",
        f"APP_BUILD_SCRIPT={android_mk}",
        f"NDK_APPLICATION_MK={app_mk}"
    ]

    res = subprocess.run(build_args)
    if res.returncode != 0:
        print(f"\n[ERROR] Compilation failed with exit code {res.returncode}.")
        sys.exit(res.returncode)

    # Copy output binary
    built_bin = os.path.join(cur_dir, "libs", "arm64-v8a", "roxy_reader")
    dest_bin = os.path.join(cur_dir, "roxy_reader")
    if os.path.exists(built_bin):
        shutil.copy2(built_bin, dest_bin)
        print("\n====================================================")
        print(f"[SUCCESS] roxy_reader compiled successfully!")
        print(f"Output binary: {dest_bin}")
        print("You can now run: python dps_meter.py")
        print("====================================================")
    else:
        print(f"\n[WARNING] Build finished but {built_bin} was not found.")

if __name__ == "__main__":
    main()
