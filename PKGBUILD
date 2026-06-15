# Maintainer: Timo Hubois <hi@pixelsaft.wtf>
pkgname=uniwill-ibg10-fanctl-dkms
pkgver=0.2.1
pkgrel=1
pkgdesc="Silent fan control for TUXEDO InfinityBook Pro Gen10 (DKMS, uniwill_ibg10_fanctl)"
arch=('x86_64')
url="https://github.com/timohubois/tuxedo-infinitybook-gen10-fan"
license=('GPL2')
depends=('dkms')
makedepends=('gcc')
source=("uniwill_ibg10_fanctl.c"
        "dkms.conf"
        "Makefile"
        "daemon/uniwill_ibg10_fanctl.c"
        "uniwill-ibg10-fanctl.service"
        "daemon/Makefile")
sha256sums=('6c88e3d53ab6a7f6453245c437858aa7d47a85f30d25e07ada39a129e468612e'
            '567d7121e661664b664646b8d8b4b1ef0f74e40889161531dd2074816410d2c7'
            'ba34b19117b49e74942cc1f9471139d20da3cab7b59fa5b368f9568a882cc1e0'
            'c2f153a2cc8708dd6266c97f0fbe9e24366c3317d2e949b1a12e303d108bf28e'
            '74057e9afcf6d1831069eb97e706a0aa6e4a8424e846bb92d7970b2b79f61813'
            '91a22a5b781fccfbac390e0bdd63f70c031d09bb143cd31c7a12b0d54a19bf66')


_dkms_name="uniwill-ibg10-fanctl"

build() {
    make -C daemon
}

package() {
    # Install DKMS module source
    install -Dm644 uniwill_ibg10_fanctl.c "$pkgdir/usr/src/$_dkms_name-$pkgver/uniwill_ibg10_fanctl.c"
    install -Dm644 dkms.conf "$pkgdir/usr/src/$_dkms_name-$pkgver/dkms.conf"
    
    # Install Makefile for DKMS (simplified version for DKMS builds)
    install -Dm644 /dev/stdin "$pkgdir/usr/src/$_dkms_name-$pkgver/Makefile" << 'EOF'
obj-m += uniwill_ibg10_fanctl.o
EOF
    
    # Install daemon
    install -Dm755 daemon/uniwill_ibg10_fanctl "$pkgdir/usr/bin/uniwill_ibg10_fanctl"
    
    # Install systemd service
    install -Dm644 uniwill-ibg10-fanctl.service "$pkgdir/usr/lib/systemd/system/uniwill-ibg10-fanctl.service"
    sed -i 's|ExecStart=.*|ExecStart=/usr/bin/uniwill_ibg10_fanctl|' "$pkgdir/usr/lib/systemd/system/uniwill-ibg10-fanctl.service"
    
    # Install module load config
    install -Dm644 /dev/stdin "$pkgdir/usr/lib/modules-load.d/uniwill-ibg10-fanctl.conf" <<< "uniwill_ibg10_fanctl"
}
