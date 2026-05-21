# Tamp — low-memory DEFLATE-inspired compressor (https://github.com/BrianPugh/tamp)
# Vendored C library (tamp/_c_src/tamp). On xtensa the embedded match path in
# compressor.c is used (no TAMP_ESP32 -> no external find_best_match needed);
# compressor_find_match_desktop.c is #included by compressor.c only on x86/arm64
# host builds, so it is NOT a standalone SRC.
idf_component_register(
        SRCS
            tamp/_c_src/tamp/common.c
            tamp/_c_src/tamp/compressor.c
            tamp/_c_src/tamp/decompressor.c
        INCLUDE_DIRS
            tamp/_c_src/tamp
)

project(tamp)
