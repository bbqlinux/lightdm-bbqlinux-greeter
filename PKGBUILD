# Maintainer: Daniel Hillenbrand <codeworkx [at] bbqlinux [dot] org>

pkgname=lightdm-bbqlinux-greeter
pkgver=2.0.0
pkgrel=1
epoch=1
pkgdesc="GTK+ greeter for LightDM"
arch=('i686' 'x86_64')
url="https://github.com/bbqlinux/lightdm-bbqlinux-greeter"
license=('GPL3' 'LGPL3')
depends=('gtk3' 'lightdm')
makedepends=('gnome-common' 'gnome-doc-utils' 'gobject-introspection'
             'intltool')
replaces=('lightdm-gtk2-greeter' 'lightdm-gtk3-greeter')
backup=('etc/lightdm/lightdm-gtk-greeter.conf')
install=$pkgname.install

build() {
  cd "${srcdir}"

  ./configure \
    --prefix='/usr' \
    --libexecdir='/usr/lib/lightdm' \
    --sbindir='/usr/bin' \
    --sysconfdir='/etc' \
    --with-libxklavier \
    --disable-libido \
    --disable-libindicator \
    --disable-static

  make
}

package() {
  cd "${srcdir}"
  make DESTDIR="${pkgdir}" install
}
