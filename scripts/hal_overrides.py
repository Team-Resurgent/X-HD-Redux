# PlatformIO pre-build script: build project-local copies of selected STM32Cube HAL
# drivers instead of the framework package's own. The stm32cube builder compiles every
# HAL driver with a fixed source filter, so the package copy is dropped here via a build
# middleware and the local copy in src/shared/stm32f0/ (already in both envs'
# build_src_filter) is compiled in its place.
#
#   stm32f0xx_hal_i2c.c -- XHD_HAL_I2C_NO_DMA: unlinks never-used I2C DMA ISRs.
Import("env")

OVERRIDDEN = ("stm32f0xx_hal_i2c.c",)

def _skip_package_copy(node):
    # The node arrives under its build-variant path (.pio/build/<env>/FrameworkHALDriver/
    # Src/...), not the package path, so match either form.
    path = node.get_path().replace("\\", "/")
    in_hal = "FrameworkHALDriver/Src/" in path or "STM32F0xx_HAL_Driver/Src/" in path
    if in_hal and path.rsplit("/", 1)[-1] in OVERRIDDEN:
        return None   # drop the framework package's copy
    return node

env.AddBuildMiddleware(_skip_package_copy)
