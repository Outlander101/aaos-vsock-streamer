# AGL-Cluster Dashboard Video Rendering (VSOCK RTSP)  

This guide explains how to launch the **Automotive Grade Linux (AGL)** VM using **QEMU** on **AArch64 architecture** for IVI-Meter Interface

It includes:
- How to create an AGL Yocto layer
- How to clone and patch the cluster dashboard QML correctly
- How to wire the patch into Yocto via `.bbappend`
- How to build and launch AGL VM

---

## Final Services present after build
Inside AGL:
- A systemd service: `vsock-rtsp-fram-bridge.service`
- An RTSP URL exposed locally in AGL:  
 `rtsp://127.0.0.1:8554/test`
Cluster dashboard:
- QML is patched so the center rectangle block uses a GStreamer/RTSP player pipeline to show the RTSP video.
Host side:
- A sender that connects to AGL via **VSOCK** and streams **FRAM-framed (custom message framing) H264** into AGL.

---

## Prerequisites
### On the build machine (Yocto host)
Install typical AGL build deps:
- git, repo
- gcc, g++, make
- python3 + common modules
- chrpath, diffstat, gawk, texinfo
- build essentials (Yocto prerequisites)
- QEMU installed with **AArch64** and **KVM** support.
- Access to `/dev/kvm` (for hardware acceleration).
- Access to `/dev/vsock` (for vsock communication).
- noVNC git and setup
```bash
cd <path to noVNC>
$ ./utils/novnc_proxy --vnc localhost:5900 --listen 6080
```

---

### On AGL runtime image (target)
Make sure these are included in the image:
- gstreamer1.0
- gstreamer plugins (base/good/bad/ugly depending on the decoder)
- gstreamer-rtsp-server
- Wayland + weston (already present in cluster image)

---
## Clone AGL and set up build env
Execute below commands to sync AGL source code for branch: **trout**.   
```bash
$ mkdir -p ~/agl
$ cd ~/agl
$ repo init -u https://gerrit.automotivelinux.org/gerrit/AGL/AGL-repo.git -b trout -m default.xml
$ repo sync
# --- Enter the build environment, example for arm64,
$ source meta-agl/scripts/aglsetup.sh -m qemuarm64 -b qemuarm64 agl-demo agl-devel
```

## Create the custom Yocto layer (meta-render)
Create a layer for:
" the vsock bridge recipe + systemd service
```bash
$ cd ~/agl
$ bitbake-layers create-layer meta-render
# Add layer to build
$ bitbake-layers add-layer ../meta-render
# Verify
$ bitbake-layers show-layers | grep meta-render
# Add the required changes provided into the layer meta-render
The core logic and files are part of firectory meta-render/recipes-core/vsock-rtsp-fram-bridge , comprising of the C code and the .service file resposible for the systemd service.
```

---

## Add vsock-rtsp-fram-bridge into Yocto (high level steps)
Inside meta-render, we will have a recipe that:

a. builds/install the vsock-rtsp-fram-bridge binary into /usr/bin/

b. Installs vsock-rtsp-fram-bridge.service into systemd

c. Enables it

For adding it to the image

Example via local.conf:
```bash

```bash
echo '# --- Multimedia for cluster-dashboard (QML: import QtMultimedia) ---
IMAGE_INSTALL:append = " \
  qtmultimedia \
  qtmultimedia-qmlplugins \
  gstreamer1.0 \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-libav \ 
  gstreamer1.0-plugins-bad \
  gstreamer1.0-rtsp-server \
"
LICENSE_FLAGS_ACCEPTED:append = " commercial "' >> conf/local.conf"
```
echo 'IMAGE_INSTALL:append = " vsock-rtsp-fram-bridge "' >> conf/local.conf"
```

## Patching the Cluster Dashboard QML

### Clone the cluster-dashboard source (outside Yocto build tree)
```bash
$ cd ~/agl
$ git clone https://gerrit.automotivelinux.org/gerrit/apps/agl-cluster-demo-dashboard.git  
$ cd agl-cluster-demo-dashboard
```

### Modify the QML file

Edit the QML file that defines the center rectangle block.

Updating QML changes :

a. Add a video item/player component

b. Point it to the RTSP URL

c. Ensure it is constrained to the rectangle and scaled as desired

Please Refer updated cluster-dashboard.qml file provided as part of patch to be updated with sample cluster-dashboard.qml file (agl-cluster-demo-dashboard/app/cluster-dashboard.qml) 
Once the changes are updated in agl-cluster-demo-dashboard, create a git patch for the changes.
```bash
$ git add <changed files>
$ git commit
$ git format-patch HEAD~1
```

## Bring the patch into the Yocto layer and apply via .bbappend
### Copy patch into the Yocto layer

For applying any patches append the patches in file .bbappend file and move the patches into meta-agl-demo/recipes-demo/cluster-dashboard/cluster-dashboard 
```bash
$ cp /path to agl-cluster-demo-dashboard/app/*.patch /path to meta-agl-demo/recipes-demo/cluster-dashboard/cluster-dashboard/
$ touch /path to meta-agl-demo/recipes-demo/cluster-dashboard/cluster-dashboard_git.bbappend
$ echo 'FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI:append = " \
file://<patch files in /path to meta-agl-demo/recipes-demo/cluster-dashboard/cluster-dashboard/> \
"' >> /path to meta-agl-demo/recipes-demo/cluster-dashboard/cluster-dashboard_git.bbappend"
```

## Build the full AGL image
```bash
$ bitbake agl-cluster-demo-qt
```

## Enable and start the vsock bridge service on AGL

On AGL:
```bash
$ systemctl daemon-reload
$ systemctl enable vsock-rtsp-fram-bridge.service
$ systemctl restart vsock-rtsp-fram-bridge.service
$ systemctl status vsock-rtsp-fram-bridge.service --no-pager
$ journalctl -u vsock-rtsp-fram-bridge.service -f
```
You should see:

RTSP ready: rtsp://127.0.0.1:8554/test and  VSOCK listening on port 5000


## Verify dashboard integration
Now launch the cluster dashboard (or let it auto-start).
If QML patch is correct, the center rectangle should connect to:
rtsp://127.0.0.1:8554/test

And If video is visible , End-to-end functionality is confirmed.

## Setup AGL Image Directory
1. Create a directory for AGL images.
2. Unzip the build package (e.g., `agl-image-qemuarm64.zip`) on an **AArch64 Graviton instance**.
3. Ensure the directory contains essential files:
   ```
   Image
   agl-cluster-demo-qt-qemuarm64.rootfs.ext4
   ```

## Launch AGL VM
```bash
qemu-system-aarch64   -machine virt,accel=kvm   -cpu host   -m 16692   -smp 16 \
-device virtio-gpu-pci   -device virtio-keyboard-pci   -device virtio-mouse-pci \
-netdev user,id=net0,hostfwd=tcp::2222-:22,hostfwd=udp::5004-:5004 -device virtio-net-pci,netdev=net0 \
-drive if=none,file="</path/to/rootfs.ext4>",format=raw,id=hd0  \
-device virtio-blk-pci,drive=hd0   -kernel "</path/to/Image>"   -append 'console=ttyAMA0 root=/dev/vda rw rootwait' \
-display vnc=127.0.0.1:0   -device vhost-vsock-pci,guest-cid=5   -serial mon:stdio 
```

---

## Access Options
- **Web VNC**: `http://<localhost>:6080/vnc.html`
- **SSH**: `ssh -p 2222 root@localhost`

---

## Key QEMU Options Explained
| **Option**                                 | **Description**                                                  |
|--------------------------------------------|------------------------------------------------------------------|
| `qemu-system-aarch64` | Launch QEMU for ARM64 architecture |
| `-machine virt,accel=kvm` | Use virt machine type with KVM acceleration |
| `-cpu host` | Emulate host CPU |
| `-m 16692` | Allocate 16 GB RAM for video processing |
| `-smp 16` | 16 virtual CPUs for video processing |
| `-device virtio-*` | Add VirtIO devices (GPU, keyboard, mouse, network) |
| `-device vhost-vsock-pci,guest-cid=5` | Enable VSOCK communication |
| `-netdev user,id=net0,hostfwd=` | Forward SSH and noVNC port |
| `-drive ...` | Attach rootfs image |
| `-kernel ...` | Load kernel image |
| `-append ...` | Kernel boot args |
| `-vnc` | Enable VNC |
| `-serial mon:stdio` | Redirect serial output |

---






