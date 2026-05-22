sudo apt install build-essential gcc meson ninja-build pkg-config libusb-1.0-0-dev

raylib dirs in meson.build anpassen

99-oszilloskop.rules anwenden oder Programm mit sudo starten

meson setup build

meson setup build-release

ninja -C build

ninja -C build-release

(sudo) ./build/oszilloskop_gui
