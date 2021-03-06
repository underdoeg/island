# Installation instructions using Vulkan SDK < `1.1.92.0`

Prior to version 1.1.92.0, the Vulkan SDK was not available as an
Ubunutu package. Setting up a working development environment was
slightly more cumbersome, and setting up dependencies for Island
required some extra steps, which are documented here if only for
nostalgia.

## Legacy Vulkan SDK installation instructions

What follows is a step-by-step guide for setting up a working Vulkan
SDK under Ubuntu 18.0 and prior.

Download Vulkan SDK from the LunarG [web
site](https://vulkan.lunarg.com/sdk/home#linux)

### Recommended SDK local folder structure

We recommend to extract this and future Vulkan SDK archives into a shared
top-level folder so that you can recreate the following structure:

    VulkanSDK/
        1.1.73.0/
        1.1.77.0/
        @current -> 1.1.77.0

Note that the folder has a symlink, `current`, which points at the
very latest version of the Vulkan SDK. This way, upgrading the SDK
becomes trivial.

### Make Vulkan SDK environment variables visible

Current linux distributions of the Vulkan SDK include a file named
`setup-env.sh`. This file needs to be sourced into your shell on
startup. 

#### Ubuntu 18.0

To source `setup-env.sh`, add the following line to your `~/.profile`:

    `source ~/Documents/VulkanSDK/current/setup-env.sh`

This assumes you have created the VulkanSDK top level folder in
`~/Documents`. If not, update the path to `setup-env.sh` accordingly.
Note that we're using the symlink `current` mentioned above, so that
we're always sourcing the current SDK.

Then add a library search path entry for SDK libs: 

    sudo echo echo "$VULKAN_SDK/lib" > /etc/ld.so.conf.d/vk.conf

Rebuild the library search cache

    ldconfig 

You should then see the correct library paths for the SDK validation
layers, if you issue:

    ldconfig -p | grep libVk

Unfortunately ubuntu won't let `setup-env.sh` update the system-wide
library search paths, `LD_LIBRARY_PATH`, which is why we have to jump
through this hoop.

### Build Vulkan SDK Tools 

Unfortunately, we can't use the binary shaderC distribution straight
from the SDK, as it was not compiled with the build flags we need for
Island. Not a big deal, we have to set our own build flags and
recompile the library:

Move to the current Vulkan SDK directory, and edit `build_tools.sh`

In method `buildShaderc()` change the build type so that it says: 

    `-DCMAKE_BUILD_TYPE=Release ..`

This is so that the build does not create a 400MB leviathan of a debug
symbol laden library, but a lean, 12MB release version. Note that
building SDK tools creates both static and dynamic version of the
shaderC library, but only adds a link to the static version of the
library to the artifacts folder. Let this build create a symlink to
the dynamic version of the shaderc library. For this, below the line:
    
    ln -sf "$PWD"/libshaderc/libshaderc_combined.a "${LIBDIR}"/libshaderc

add the line:

    ln -sf "$PWD"/libshaderc/libshaderc_shared.so "${LIBDIR}"/libshaderc

Save & close `build_tools.sh`, then build the SDK tools:

    ./build_tools.sh


