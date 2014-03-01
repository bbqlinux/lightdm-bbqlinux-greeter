# Maintainer: Daniel Hillenbrand <codeworkx [at] bbqlinux [dot] org>

pkgname=lightdm-bbqlinux-greeter
pkgver=1.8.1
pkgrel=1
pkgdesc="GTK2+ greeter for LightDM"
arch=('i686' 'x86_64')
url="https://github.com/bbqlinux/lightdm-bbqlinux-greeter"
license=('GPL3' 'LGPL3')
depends=('gtk2' 'lightdm>=1.6.0' 'gtk-theme-bbqlinux')
makedepends=('exo' 'gnome-doc-utils' 'gobject-introspection' 'intltool')
conflicts=('lightdm-gtk-greeter' 'lightdm-gtk2-greeter-devel')
backup=('etc/lightdm/lightdm-bbqlinux-greeter.conf')

build() {
  cd "${srcdir}"

  ./configure --prefix=/usr --sbindir=/usr/bin --sysconfdir=/etc --libexecdir=/usr/lib/lightdm --disable-static --with-gtk2
  make
}

package() {
  cd "${srcdir}"

  make DESTDIR="${pkgdir}" install
}
