#!/bin/bash
# build.sh - builds PsiBoost into build/pkg/PsiBoost.sis: the program, its
# helper, the UART driver, the hook and the start-up recogniser. Run it in
# psion-core's build container, from this folder:
#   core/tools/docker/psibuild "bash build.sh"
# (psion-core is the submodule core/; PSION_SDK is set in the container.)
set -e
: "${PSION_SDK:?set PSION_SDK to your psion_cpp_sdk_linux directory}"
HERE=$(cd "$(dirname "$0")" && pwd)
CORE=${PSION_CORE:-$HERE/core}
[ -f "$CORE/ssh/psilink.h" ] || CORE=$HERE/../core
[ -f "$CORE/ssh/psilink.h" ] || { echo "psion-core not found: git submodule update --init"; exit 1; }
OUT=$HERE/build
export EPOCROOT=$PSION_SDK/epoc_cpp_sdk
REL=$EPOCROOT/epoc32/release/marm/rel
export PATH=/usr/bin:/bin:$PSION_SDK/gcc-3.0-psion-98r2-9/bin:$EPOCROOT/epoc32/tools
export TMP=$EPOCROOT/tmp WINEDEBUG=-all
mkdir -p "$TMP" "$EPOCROOT/ptproj" "$OUT"

echo "== psiekern.lib (EKERN imports with the 5mx ROM's ordinals)"
# (in a scratch folder with TMP=.: with the SDK's TMP, dlltool names the
# archive members "tmp\\d11..." and the library comes out unusable)
D=$OUT/implib; rm -rf "$D"; mkdir -p "$D"
( cd "$D" && TMP=. arm-pe-dlltool --as=arm-pe-as --output-lib "$REL/psiekern.lib" \
	--def "$HERE/ekern_rom.def" --dllname "EKERN[100000ba].EXE" )

# the SDK finds each project through a link in its ptproj/ folder
for l in psiboost:. psiboostcore:$CORE/ssh pboosthook:hook pboostrec:rec pboostuart:uart; do
	name=${l%%:*}; dir=${l#*:}
	case $dir in /*) ;; *) dir=$HERE/$dir ;; esac
	if [ -e "$EPOCROOT/ptproj/$name" ] && [ ! -L "$EPOCROOT/ptproj/$name" ]; then
		echo "$EPOCROOT/ptproj/$name exists and is not a link - move it away"; exit 1
	fi
	ln -sfn "$(cd "$dir" && pwd)" "$EPOCROOT/ptproj/$name"
done

mk() {   # mk PROJECT MMP LOG
	( cd "$EPOCROOT/ptproj/$1"
	  makmake "$2" marm > "$OUT/$3-makmake.log" 2>&1
	  make -f "$2.marm" rel > "$OUT/$3.log" 2>&1 \
		|| { grep -i -B1 -A4 "error\|undefined" "$OUT/$3.log" | grep -v "def file" | head -40; exit 1; } )
}

echo "== PSIBOOST.APP, PBOOSTD.EXE"
python3 "$HERE/mktbar.py" "$OUT/icons" "$HERE/spicons.h"
( cd "$OUT/icons" && wine "$EPOCROOT/epoc32/tools/bmconv.exe" /q psiboost.mbm $(tr '\n' ' ' < files.txt) > /dev/null 2>&1 )
mk psiboost psiboost app
mk psiboost pboostd helper
echo "== PBUART21.LDD, PBHOOK20.LDD, PsiBoost.rdl"
mk pboostuart pbuart uart
mk pboosthook pbhook hook
mk pboostrec pbrec rec

P=$OUT/pkg
rm -rf "$P"; mkdir -p "$P"
cp "$REL/pbuart21.ldd" "$REL/pbhook20.ldd" "$REL/psiboost.rdl" "$REL/psiboost.app" "$REL/psiboost.rsc" \
	"$REL/pboostd.exe" "$HERE/psiboost.pkg" "$OUT/icons/psiboost.mbm" "$P/"
python3 "$HERE/mkicon.py" "$P/psiboost.aif" > /dev/null
( cd "$P" && wine "$EPOCROOT/epoc32/tools/makesis.exe" psiboost.pkg PsiBoost.sis > /dev/null )
ls -la "$P/PsiBoost.sis"
