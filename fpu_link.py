Import("env")

# PlatformIO passes -mfpu/-mfloat-abi from build_flags to the compiler only;
# the link step needs them too so the hard-float libs are selected.
env.Append(LINKFLAGS=["-mfpu=fpv4-sp-d16", "-mfloat-abi=hard"])
