set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(CMAKE_INSTALL_LIBDIR "lib/aarch64-linux-gnu" CACHE PATH "Multiarch libdir")

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_AR /usr/bin/aarch64-linux-gnu-gcc-ar)
set(CMAKE_RANLIB /usr/bin/aarch64-linux-gnu-gcc-ranlib)
set(CMAKE_C_COMPILER_AR /usr/bin/aarch64-linux-gnu-gcc-ar)
set(CMAKE_C_COMPILER_RANLIB /usr/bin/aarch64-linux-gnu-gcc-ranlib)

set(CORTEX_A53_FLAGS "-O3 -DNDEBUG -mcpu=cortex-a53 -mtune=cortex-a53 -flto=auto -fomit-frame-pointer -fno-math-errno -fno-trapping-math")
set(CMAKE_C_FLAGS_RELEASE "${CORTEX_A53_FLAGS}" CACHE STRING "Release C flags" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-flto=auto" CACHE STRING "Release exe linker flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "-flto=auto" CACHE STRING "Release shared linker flags" FORCE)

set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")

set(CMAKE_FIND_ROOT_PATH
  /usr/aarch64-linux-gnu
  /usr/lib/aarch64-linux-gnu
  /usr/include/aarch64-linux-gnu
  /usr
)
set(CMAKE_INCLUDE_PATH /usr/include/aarch64-linux-gnu /usr/include)
set(CMAKE_LIBRARY_PATH /usr/lib/aarch64-linux-gnu)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
