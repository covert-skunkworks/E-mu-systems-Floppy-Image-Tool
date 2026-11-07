#!/usr/bin/env bash
# Pack a built floppy_image for release: the program, README.txt and the licence notices of what it carries (Nuklear, the
# Droid Sans font).
#
# usage, from the project's root after make:
#   scripts/package.sh OS VERSION [PROGRAM]     OS: windows, linux or macos
#
# Writes into dist/:
#
#   floppy_image-VERSION-windows-x64.zip       floppy_image.exe, statically linked
#   floppy_image-VERSION-linux-x64.tar.gz      tar keeps the program executable
#   floppy_image-VERSION-macos-universal.zip   floppy_image.app (arm64 + x86_64), made with ditto, which keeps the
#                                              bundle's signature and links
#
# Each holds one folder of the same name. The font's licence (Apache 2.0) has to travel with it; its text is fetched from
# apache.org unless APACHE_LICENSE names a local copy.

set -euo pipefail

os=${1:?usage: package.sh OS VERSION [PROGRAM]}
ver=${2:?usage: package.sh OS VERSION [PROGRAM]}
case "$os" in
	windows) arch=x64; prog=${3:-floppy_image.exe} ;;
	linux) arch=x64; prog=${3:-floppy_image} ;;
	macos) arch=universal; prog=${3:-floppy_image} ;;
	*) echo "package.sh: unknown OS $os" >&2; exit 1 ;;
esac
name="floppy_image-$ver-$os-$arch"
out="dist/$name"

# The stand-in drive belongs to the test builds. A program that lists it was built wrongly.
if grep -aq 'Test drive (a file' "$prog"; then
	echo "package.sh: $prog is a test build: it has the stand-in drive" >&2
	exit 1
fi

rm -rf "$out" "dist/$name.zip" "dist/$name.tar.gz"
mkdir -p "$out/licenses"

case "$os" in
	windows) cp "$prog" "$out/floppy_image.exe" ;;
	linux) cp "$prog" "$out/floppy_image" ;;
	macos)
		app="$out/floppy_image.app"
		mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
		cp "$prog" "$app/Contents/MacOS/floppy_image"
		cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>                  <string>floppy_image</string>
  <key>CFBundleDisplayName</key>           <string>floppy_image</string>
  <key>CFBundleExecutable</key>            <string>floppy_image</string>
  <key>CFBundleIdentifier</key>            <string>local.floppy-image</string>
  <key>CFBundlePackageType</key>           <string>APPL</string>
  <key>CFBundleShortVersionString</key>    <string>$ver</string>
  <key>CFBundleVersion</key>               <string>$ver</string>
  <key>CFBundleIconFile</key>              <string>AppIcon</string>
  <key>LSMinimumSystemVersion</key>        <string>11.0</string>
  <key>NSHighResolutionCapable</key>       <false/>
</dict>
</plist>
EOF
		# the Finder icon from the Windows icon; the app works without it if this fails
		icons="$out.iconset"
		( sips -s format png res/app.ico --out "$out.png" > /dev/null &&
			mkdir -p "$icons" &&
			for s in 16 32 128 256; do
				sips -z $s $s "$out.png" --out "$icons/icon_${s}x${s}.png" > /dev/null
			done &&
			iconutil -c icns "$icons" -o "$app/Contents/Resources/AppIcon.icns" ) || echo "package.sh: icon skipped"
		rm -rf "$icons" "$out.png"
		codesign --force --deep --sign - "$app"
		;;
esac

cp release/README.txt "$out/README.txt"
cp third_party/nuklear/LICENSE "$out/licenses/Nuklear.txt"
if [ -f LICENSE ]; then
	cp LICENSE "$out/licenses/floppy_image.txt"
fi
if [ -n "${APACHE_LICENSE:-}" ]; then
	cp "$APACHE_LICENSE" "$out/licenses/DroidSans-Apache-2.0.txt"
else
	curl -fsSL https://www.apache.org/licenses/LICENSE-2.0.txt -o "$out/licenses/DroidSans-Apache-2.0.txt"
fi

cd dist
case "$os" in
	windows)
		echo "dist/$name.zip:"
		if command -v zip > /dev/null; then
			zip -qr "$name.zip" "$name"
		else
			powershell.exe -NoProfile -Command "Compress-Archive -Path '$(cygpath -w "$PWD/$name")' -DestinationPath '$(cygpath -w "$PWD/$name.zip")'"
		fi
		;;
	linux)
		echo "dist/$name.tar.gz:"
		tar czf "$name.tar.gz" "$name"
		;;
	macos)
		echo "dist/$name.zip:"
		ditto -c -k --keepParent "$name" "$name.zip"
		;;
esac
find "$name" | sort
rm -rf "$name"
