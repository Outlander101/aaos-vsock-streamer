FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI:append = " \
  file://0001-feat-Modify-cluster-dashboard-to-be-the-rtsp-client-.patch \
"
