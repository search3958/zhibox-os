# Vendored BaramOS GUI sources

This directory contains the GUI crates and runtime files copied into zhibox-os
so its Linux desktop build is self-contained. Build scripts must use this tree
and must not fetch source from a sibling `baram-os` checkout.

Linux-specific adaptations live in `scripts/guest/gradient/` and are applied
by `scripts/build-desktop.sh` while assembling `output/build/desktop-src`.
The vendored `baram-windowserver` sources include zhibox-os fixes directly.
