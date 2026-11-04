
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "git://github.com/JayanthBalan/Custom-Linux-Kernel-Driver-for-SPI-Based-RFID-Integration.git;protocol=https;branch=main"

PV = "1.0+git${SRCPV}"
SRCREV = "e2ac2b9361a0942c070864e8e6f5df7e1f9d56b4"

S = "${WORKDIR}/git/source-code"

inherit module update-rc.d

FILES:${PN} += "${bindir}/aeld-cdev-load ${bindir}/aeld-cdev-unload"

INITSCRIPT_PACKAGES = "${PN}"
INITSCRIPT_NAME:${PN} = "aeld-final-start-stop"
INITSCRIPT_PARAMS:${PN} = "defaults 95"

FILES:${PN} += "${sysconfdir}/init.d/aeld-final-start-stop"

SRC_URI += "file://aeld-final-start-stop"

EXTRA_OEMAKE:append:task-install = " -C ${STAGING_KERNEL_DIR} M=${S}"
EXTRA_OEMAKE += "KERNELDIR=${STAGING_KERNEL_DIR}"

do_install:append() {
    install -d ${D}${sysconfdir}/init.d
    install -m 0755 ${WORKDIR}/aeld-final-start-stop ${D}${sysconfdir}/init.d/aeld-final-start-stop

    install -d ${D}${bindir}
	install -m 0755 ${S}/aeld-cdev-unload ${D}${bindir}/aeld-cdev-unload
    install -m 0755 ${S}/aeld-cdev-load ${D}${bindir}/aeld-cdev-load
}
