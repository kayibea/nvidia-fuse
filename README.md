# NVIDIA File System

Share Nvidia GPU stats through a fuse file system

# Build
```bash
sudo apt install libfuse3-dev libnvidia-ml-dev
make
```

Mount point: `/tmp/nvfs`
Contains files:
* `temp`  - GPU temperature
* `vram`  - VRAM usage percentage
* `util`  - GPU utilization percentage
