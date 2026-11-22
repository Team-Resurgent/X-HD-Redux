Import("env")

"""
Combined script for combined_stm32f0 environment.
Builds bootloader and application, then combines them into a single binary.
"""

print("DEBUG: combine_firmware.py script loaded")

# Bootloader size in bytes (20KB); padded to this size
BOOTLOADER_SIZE = 20 * 1024
# App region max size for sanity check (43KB); no padding
# 43KB, not the full 44KB up to page 63, to keep this check catching an app image that's
# grown into flash page 63 (0x0800FC00-0x0800FFFF, reserved at runtime for user_config's
# persistent storage) at build time instead of silently corrupting it at flash time --
# see the matching NOTE in src/application/application.ld.
APP_SIZE_BYTES = 43 * 1024
PADDING_VALUE = 0xFF  # Typical flash erase value


def build_and_combine(target, source, env):
    """Build bootloader and application, then combine them."""
    import subprocess
    import sys
    import shutil
    from pathlib import Path
    
    # Only run if we're building the combined environment
    env_name = env.get("PIOENV", "")
    print(f"DEBUG: build_and_combine called, PIOENV: {env_name}")
    if env_name != "combined_stm32f0":
        print(f"DEBUG: Skipping - PIOENV is '{env_name}', not 'combined_stm32f0'")
        return
    
    # This function is called as a SCons action, so target/source might be SCons nodes
    # We can ignore them and just do our work
    
    project_dir = env["PROJECT_DIR"]
    
    print("=" * 60)
    print("Building bootloader and application, then combining...")
    print("=" * 60)
    
    # Build bootloader and application
    try:
        pio_cmd = shutil.which("platformio") or shutil.which("pio")
        if not pio_cmd:
            pio_base = [sys.executable, "-m", "platformio"]
        else:
            pio_base = [pio_cmd]
        
        # Build bootloader first
        print("\n[1/3] Building bootloader (bootloader_stm32f0)...")
        result = subprocess.run(
            pio_base + ["run", "-e", "bootloader_stm32f0"],
            check=True,
            cwd=project_dir
        )
        print("✓ Bootloader built successfully!")
        
        # Build application
        print("\n[2/3] Building application (application_stm32f0)...")
        result = subprocess.run(
            pio_base + ["run", "-e", "application_stm32f0"],
            check=True,
            cwd=project_dir
        )
        print("✓ Application built successfully!")
    except subprocess.CalledProcessError as e:
        print(f"✗ Error: Failed to build (return code: {e.returncode})")
        sys.exit(1)
    except Exception as e:
        print(f"✗ Error building: {e}")
        sys.exit(1)
    
    # Now combine the binaries
    print("\n[3/3] Combining binaries...")
    build_dir = Path(".pio/build")
    
    bootloader_bin = build_dir / "bootloader_stm32f0" / "firmware.bin"
    application_bin = build_dir / "application_stm32f0" / "firmware.bin"
    
    # Output to combined_stm32f0 folder
    output_dir = build_dir / "combined_stm32f0"
    output_bin = output_dir / "firmware.bin"
    
    # Delete contents of combined_stm32f0 folder before writing
    if output_dir.exists():
        print(f"Cleaning output directory: {output_dir}")
        for item in output_dir.iterdir():
            if item.is_file():
                item.unlink()
            elif item.is_dir():
                shutil.rmtree(item)
    else:
        output_dir.mkdir(parents=True, exist_ok=True)
    
    # Check if bootloader binary exists
    if not bootloader_bin.exists():
        print(f"✗ Error: Bootloader binary not found: {bootloader_bin}")
        sys.exit(1)
    
    # Check if application binary exists
    if not application_bin.exists():
        print(f"✗ Error: Application binary not found: {application_bin}")
        sys.exit(1)
    
    # Read bootloader binary
    print(f"Reading bootloader: {bootloader_bin}")
    with open(bootloader_bin, 'rb') as f:
        bootloader_data = f.read()
    
    bootloader_size = len(bootloader_data)
    print(f"Bootloader size: {bootloader_size} bytes")
    
    if bootloader_size > BOOTLOADER_SIZE:
        print(f"✗ Error: Bootloader size ({bootloader_size} bytes) exceeds {BOOTLOADER_SIZE} bytes!")
        sys.exit(1)
    
    # Pad bootloader to 20KB
    padding_size = BOOTLOADER_SIZE - bootloader_size
    if padding_size > 0:
        print(f"Padding bootloader with {padding_size} bytes (0xFF)")
        bootloader_data += bytes([PADDING_VALUE] * padding_size)
    
    # Read application binary
    print(f"Reading application: {application_bin}")
    with open(application_bin, 'rb') as f:
        application_data = f.read()
    
    # Stamp the image header (src/shared/app_header.h): magic, length, then the CRC-32 of
    # the image with the crc field left out. The linker leaves it all ones right after the
    # vector table; anything else there means the layout moved, and the check would fail.
    import struct
    import zlib
    HDR_OFF, HDR_MAGIC = 0xC0, 0x31485841
    app = bytearray(application_data)
    app += bytes([PADDING_VALUE] * (-len(app) % 4))
    if app[HDR_OFF:HDR_OFF + 12] != b"\xff" * 12:
        print(f"✗ Error: no blank image header at 0x{HDR_OFF:X} (found {app[HDR_OFF:HDR_OFF + 12].hex()})")
        sys.exit(1)
    struct.pack_into("<II", app, HDR_OFF, HDR_MAGIC, len(app))
    crc = zlib.crc32(bytes(app[:HDR_OFF + 8]) + bytes(app[HDR_OFF + 12:])) & 0xFFFFFFFF
    struct.pack_into("<I", app, HDR_OFF + 8, crc)
    application_data = bytes(app)
    print(f"Image header: {len(app)} bytes, CRC-32 {crc:08X}")

    application_size = len(application_data)
    print(f"Application size: {application_size} bytes")

    if application_size > APP_SIZE_BYTES:
        print(f"✗ Error: Application size ({application_size} bytes) exceeds app region size ({APP_SIZE_BYTES} bytes)!")
        sys.exit(1)
    
    print(f"App image: {application_size} bytes")
    
    # Combine binaries
    print(f"Combining binaries...")
    combined_data = bootloader_data + application_data
    
    # Write combined binary
    print(f"Writing combined binary: {output_bin}")
    with open(output_bin, 'wb') as f:
        f.write(combined_data)

    # bin/firmware.bin is the one tracked image: the latest build, nothing else is kept.
    # (Its version banner carries the commit, so a binary read back from the board maps to git.)
    bin_dir = Path(project_dir) / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    latest_bin = bin_dir / "firmware.bin"
    shutil.copy2(output_bin, latest_bin)

    # Drop-in folder for XHDUpdater.xbe: it loads firmware_<encoder>.bin from its own
    # directory, picking the name by probing the console's encoder (Utility::LoadFirmware()),
    # and flashes from offset 20KB of that file -- so it must be this combined image. The
    # image is the same for every encoder (detected at runtime), so all three names get it.
    # Copy the whole folder to the Xbox and run the .xbe from there.
    upd_dir = bin_dir / "xhdupdater"
    upd_dir.mkdir(parents=True, exist_ok=True)
    for enc in ("conexant", "focus", "xcalibur"):
        shutil.copy2(output_bin, upd_dir / f"firmware_{enc}.bin")
    updater_xbe = Path(project_dir) / "updater" / "XHDUpdater.xbe"
    if updater_xbe.exists():
        shutil.copy2(updater_xbe, upd_dir / "XHDUpdater.xbe")

    total_size = len(combined_data)
    print(f"\n✓ Combined binary created successfully!")
    print(f"  Output: {output_bin}")
    print(f"  Image: {latest_bin}")
    print(f"  XHDUpdater folder: {upd_dir}")
    print(f"  Total size: {total_size} bytes ({total_size / 1024:.2f} KB)")
    print("=" * 60)

# Create a custom target that always runs, since we're not compiling anything
env_name = env.get("PIOENV", "")
if env_name == "combined_stm32f0":
    print("DEBUG: Creating custom target for combined environment")
    from SCons.Script import AlwaysBuild, Default
    
    # Create the target that does the actual work
    combine_target = env.Command(
        env["BUILD_DIR"] + "/combined_firmware",
        [],
        build_and_combine
    )
    AlwaysBuild(combine_target)
    Default(combine_target)
    env.Alias("combined", combine_target)
