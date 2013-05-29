# Maintainer: Daniel Hillenbrand <codeworkx@bbqlinux.org>

pkgname=lightdm-bbqlinux-greeter
pkgver=1.0.0
pkgrel=1
pkgdesc="GTK+ greeter for LightDM"
arch=('i686' 'x86_64')
url="https://github.com/bbqlinux/lightdm-bbqlinux-greeter"
license=('GPL3' 'LGPL3')
depends=('gtk2' 'lightdm>=1.5.1')
makedepends=('exo' 'gnome-doc-utils' 'gobject-introspection' 'intltool')
backup=('etc/lightdm/lightdm-bbqlinux-greeter.conf')

build() {
  cd "${srcdir}"

  ./configure --prefix=/usr --sbindir=/usr/bin --sysconfdir=/etc --libexecdir=/usr/lib/lightdm --disable-static
  make
}

package() {
  cd "${srcdir}"

  make DESTDIR="${pkgdir}" install
}
