# PlatformIO post script (application env): the link around the startup file. -flto needs
# three things the build flags alone don't give it (see the -flto note in platformio.ini):
#  - -flto on the link too (build_flags only reach the compiler);
#  - the startup file (vector table, weak handler aliases) as a plain object ahead of every
#    other one: linked later -- or from the framework's library -- the startup file's weak
#    Default_Handler aliases replace every real interrupt handler;
#  - a check after the link that they didn't: that failure builds clean and gives a board
#    with no SysTick, SMBus, ADV7511 interrupt or UART.
import os
import re
import subprocess

Import("env")

startup = env.Object(os.path.join("$BUILD_DIR", "startup", "startup_stm32f030x8.o"),
                     os.path.join("$PROJECT_SRC_DIR", "shared", "stm32f0", "startup_stm32f030x8.S"))
elf = os.path.join("$BUILD_DIR", "${PROGNAME}.elf")
env.Depends(elf, startup)
env.Prepend(LINKFLAGS=[startup[0].get_abspath()])
env.Append(LINKFLAGS=["-flto"])
# No C runtime start files (crti/crtbegin/crtend/crtn): nothing has a constructor -- .init_array
# held only crtbegin's frame_dummy -- and the startup file no longer calls __libc_init_array.
# A link flag (build_flags only reach the compiler), so here with the startup object.
env.Append(LINKFLAGS=["-nostartfiles"])

# The handlers defined in src/ (USART2_IRQHandler only in the non-DMA UART build).
HANDLERS = ("SysTick_Handler", "HardFault_Handler", "I2C2_IRQHandler",
            "EXTI4_15_IRQHandler", "DMA1_Channel4_5_IRQHandler")


def _check_handlers(target, source, env):
    path = target[0].get_abspath()
    nm = re.sub(r"gcc$", "nm", env.subst("$CC"))   # only the tool name: a path may hold "gcc" too
    syms = {}
    for line in subprocess.check_output([nm, path], text=True).splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = (parts[0], parts[1])
    default = syms.get("Default_Handler", ("", ""))[0]
    bad = [h for h in HANDLERS if h not in syms or syms[h][1] != "T" or syms[h][0] == default]
    if bad:
        os.remove(path)
        print("LTO check FAILED, on Default_Handler or missing: " + ", ".join(bad))
        return 1
    print("LTO check: the %d interrupt handlers are the real ones" % len(HANDLERS))
    return 0


env.AddPostAction(elf, env.VerboseAction(_check_handlers, "Checking interrupt handlers"))
