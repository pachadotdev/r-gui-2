pkgname=simpler
pkgver=0.3.0
pkgrel=1
pkgdesc="Simpler - Made with Qt"
arch=('x86_64')
url="https://github.com/yourusername/r-gui-2"
license=('Apache-2.0')
depends=('r>=4.0.0' 'qt6-base')
makedepends=('git' 'cmake>=3.16' 'gcc')

source=()
md5sums=()

build() {
    cd "${startdir}"
    
    # Locate qmake for Qt6
    if command -v qmake-qt6 >/dev/null 2>&1; then
        _QMAKE_EXECUTABLE="$(command -v qmake-qt6)"
    elif [ -x /usr/lib/qt6/bin/qmake ]; then
        _QMAKE_EXECUTABLE=/usr/lib/qt6/bin/qmake
    else
        _QMAKE_EXECUTABLE="$(command -v qmake || echo /usr/bin/qmake)"
    fi
    
    msg "Using Qt qmake: ${_QMAKE_EXECUTABLE}"
    
    # Clean and create build directory
    rm -rf build
    mkdir -p build
    cd build
    
    # Configure
    cmake ../r-gui-2 \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
    
    # Build
    make -j$(nproc)
}

package() {
    cd "${startdir}/build"
    
    # Install the binary
    install -Dm755 bin/simpler "${pkgdir}/usr/bin/simpler"
    
    # Install desktop file
    install -Dm644 "${startdir}/simpler.desktop" "${pkgdir}/usr/share/applications/simpler.desktop"
    
    # Install icon (if exists)
    if [ -f "${startdir}/simpler.png" ]; then
        install -Dm644 "${startdir}/simpler.png" "${pkgdir}/usr/share/pixmaps/simpler.png"
    fi
}
