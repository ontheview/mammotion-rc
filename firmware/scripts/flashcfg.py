# PlatformIO extra_script: adds a `flashcfg` custom target that packs
# config.local.ini -> config.bin and writes it to the config partition via
# esptool.  Runs only when invoked explicitly:
#
#     pio run -e hc33 -t flashcfg
#     pio run -e hc33-standard-wifi -t flashcfg
#
# Regular `pio run -t upload` is unaffected; it never touches the config
# partition, so the firmware boots on the compiled config.h defaults.  See
# config_load.cpp.
#
# Offset is hardcoded here to match the `config` line in partitions/hc33.csv.
# Keep them in sync (single number, won't change unless we rev the partition
# table).

import os

# This script is loaded by PlatformIO at config-parse time.  At that point
# `env` is injected into globals; outside that context (e.g. when a linter
# tries to import the file) the reference is just undefined.
Import("env")  # noqa: F821  (provided by PIO)

CONFIG_PARTITION_OFFSET = 0x7EF000


def _flashcfg_action(*_args, **_kwargs):
    proj_dir = env.subst("$PROJECT_DIR")  # noqa: F821
    build_dir = env.subst("$BUILD_DIR")   # noqa: F821
    python_exe = env.subst("$PYTHONEXE")  # noqa: F821

    ini = os.path.join(proj_dir, "config.local.ini")
    if not os.path.isfile(ini):
        example = os.path.join(proj_dir, "config.local.ini.example")
        print(f"flashcfg: {ini} not found.")
        print(f"flashcfg: copy {example} to config.local.ini and fill it in.")
        env.Exit(1)  # noqa: F821

    packer = os.path.join(proj_dir, "scripts", "pack_config.py")
    out_bin = os.path.join(build_dir, "config.bin")
    os.makedirs(build_dir, exist_ok=True)

    rc = env.Execute(f'"{python_exe}" "{packer}" "{ini}" "{out_bin}"')  # noqa: F821
    if rc:
        env.Exit(rc)  # noqa: F821

    # Reuse the upload port PIO already knows.  If unset, esptool will
    # autodetect; we just don't pass --port in that case.
    port = env.subst("$UPLOAD_PORT")  # noqa: F821
    port_arg = f' --port "{port}"' if port else ""

    cmd = (
        f'"{python_exe}" -m esptool{port_arg} '
        f'--chip esp32s3 '
        f'write_flash 0x{CONFIG_PARTITION_OFFSET:X} "{out_bin}"'
    )
    env.Execute(cmd)  # noqa: F821


env.AddCustomTarget(  # noqa: F821
    name="flashcfg",
    dependencies=None,
    actions=[_flashcfg_action],
    title="Flash config",
    description="Pack config.local.ini and write it to the `config` partition.",
)
