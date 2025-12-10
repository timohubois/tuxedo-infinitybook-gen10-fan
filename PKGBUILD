# Maintainer: Timo Hubois <your-email@example.com>
pkgname=tuxedo-infinitybook-gen10-fan-dkms
pkgver=0.1.0
pkgrel=1
pkgdesc="Silent fan control for TUXEDO InfinityBook Pro Gen10 (DKMS)"
arch=('x86_64')
url="https://github.com/timohubois/tuxedo-infinitybook-gen10-fan"
license=('GPL2')
depends=('dkms')
makedepends=('gcc')
source=("tuxedo_infinitybook_gen10_fan.c"
        "dkms.conf"
        "Makefile"
        "ibg10-fanctl.c"
        "tuxedo-infinitybook-gen10-fan.service")
sha256sums=('SKIP'
            'SKIP'
            'SKIP'
            'SKIP'
            'SKIP')

_dkms_name="tuxedo-infinitybook-gen10-fan"

build() {
    gcc -Wall -Wextra -O2 -o ibg10-fanctl ibg10-fanctl.c
}

package() {
    # Install DKMS module source
    install -Dm644 tuxedo_infinitybook_gen10_fan.c "$pkgdir/usr/src/$_dkms_name-$pkgver/tuxedo_infinitybook_gen10_fan.c"
    install -Dm644 dkms.conf "$pkgdir/usr/src/$_dkms_name-$pkgver/dkms.conf"
    
    # Install Makefile for DKMS (simplified version for DKMS builds)
    install -Dm644 /dev/stdin "$pkgdir/usr/src/$_dkms_name-$pkgver/Makefile" << 'EOF'
obj-m += tuxedo_infinitybook_gen10_fan.o
EOF
    
    # Install daemon
    install -Dm755 ibg10-fanctl "$pkgdir/usr/bin/ibg10-fanctl"
    
    # Install systemd service
    install -Dm644 tuxedo-infinitybook-gen10-fan.service "$pkgdir/usr/lib/systemd/system/tuxedo-infinitybook-gen10-fan.service"
    
    # Install module load config
    install -Dm644 /dev/stdin "$pkgdir/usr/lib/modules-load.d/tuxedo-infinitybook-gen10-fan.conf" <<< "tuxedo_infinitybook_gen10_fan"
}
