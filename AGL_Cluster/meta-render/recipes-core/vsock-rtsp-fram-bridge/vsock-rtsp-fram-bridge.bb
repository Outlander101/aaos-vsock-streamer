SUMMARY = "VSOCK FRAM(H264) -> RTSP bridge (appsrc + gst-rtsp-server)"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
  file://vsock-rtsp-fram-bridge.c \
  file://vsock-rtsp-fram-bridge.service \
"

S = "${WORKDIR}"

inherit pkgconfig systemd

DEPENDS += "gstreamer1.0 gstreamer1.0-plugins-base gstreamer1.0-rtsp-server glib-2.0"

do_compile() {
  ${CC} ${CFLAGS} ${LDFLAGS} \
    -o vsock-rtsp-fram-bridge ${WORKDIR}/vsock-rtsp-fram-bridge.c \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0 glib-2.0)
}

do_install() {
  install -d ${D}${bindir}
  install -m 0755 ${B}/vsock-rtsp-fram-bridge ${D}${bindir}/vsock-rtsp-fram-bridge

  install -d ${D}${systemd_system_unitdir}
  install -m 0644 ${WORKDIR}/vsock-rtsp-fram-bridge.service \
    ${D}${systemd_system_unitdir}/vsock-rtsp-fram-bridge.service
}

SYSTEMD_SERVICE:${PN} = "vsock-rtsp-fram-bridge.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"
