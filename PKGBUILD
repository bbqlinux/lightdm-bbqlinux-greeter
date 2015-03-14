# Maintainer: Daniel Hillenbrand <codeworkx [at] bbqlinux [dot] org>

pkgname=lightdm-bbqlinux-greeter
pkgver=1.9.0
pkgrel=1
pkgdesc="GTK+ greeter for LightDM"
arch=('i686' 'x86_64')
url="https://github.com/bbqlinux/lightdm-bbqlinux-greeter"
license=('GPL3' 'LGPL3')
depends=('gtk2' 'lightdm>=1.6.0' 'bbqlinux-theme' 'python2')
makedepends=('exo' 'gnome-doc-utils' 'gobject-introspection' 'intltool')
conflicts=('lightdm-gtk2-greeter' 'lightdm-gtk3-greeter')
backup=('etc/lightdm/lightdm-gtk-greeter.conf')
install=$pkgname.install

build() {
  cd "${srcdir}"

  ./configure --prefix=/usr --sbindir=/usr/bin --sysconfdir=/etc --libexecdir=/usr/lib/lightdm --disable-static --with-gtk2
  make
}

package() {

  cd "${srcdir}"
  make DESTDIR="${pkgdir}" install

  cd "${pkgdir}"
  mkdir -p usr/bin
  install -Dm755 "$srcdir/usr/bin/update-lightdm-conf.py" usr/bin/update-lightdm-conf.py

}
