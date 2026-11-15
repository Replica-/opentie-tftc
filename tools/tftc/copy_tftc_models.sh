#!/usr/bin/env bash
#
# Copy the TFTC (TIE Fighter Total Conversion) ship models that match TIE98
# craft into resources/tftc/, where the HD flight renderer draws them in
# place of the original models. Each OPT and its Materials/*.mat file are
# renamed after the TIE98 OPT they replace (TieFighter.opt -> TIEFTR.opt).
# The 3D cockpits, with their .mat and .dc files, go to resources/cockpit3d/,
# the Music/*.IMC soundtrack to resources/tftc/music/, and each mission's
# skybox cubemap to resources/tftc/skyboxes/.
# The build stages resources/ next to OpenTIE.exe, so rebuild Debug or
# Release after running this. The files are not redistributable and
# resources/tftc/ is ignored by git.
#
# Usage: tools/tftc/copy_tftc_models.sh <TFTC install dir> [<output dir>]
#
# Example:
#   tools/tftc/copy_tftc_models.sh "E:/Games/xwing/TFTC-C"

set -euo pipefail

if [ "$#" -lt 1 ]; then
    sed -n '3,17p' "$0" >&2
    exit 2
fi

tftc="$1"
out="${2:-$(cd "$(dirname "$0")/../.." && pwd)/resources/tftc}"
models="$tftc/FlightModels"
materials="$tftc/Materials"
if [ ! -d "$models" ]; then
    echo "error: $models not found" >&2
    exit 1
fi
mkdir -p "$out"

# TIE98 OPT basename, TFTC FlightModels basename (from TFTC's SPACECRAFT0.LST).
mapping="
XWING      Xwing
YWING      Ywing
AWING      Awing
BWING      Bwing
TIEFTR     TieFighter
TIEINT     TieInterceptor
TIEBMB     TieBomber
TIEADV     TIEAdvancedx1
TIEDEL     TieDefender
MISLBT     MissileBoat
FGHTA      Twing
Z-95       Z-95
FGHTB      R41Starchaser
ASSAULT    AssaultGunboat
SHUTTLE    ShuttleTFTC-C
SHUTB      EscortShuttle
PATRL      SystemPatrolCraft
PATRLB     ScoutCraft
TRNSPRT    StormtrooperTransport
TRANSB     AssaultTransportTFTC-C
ESCTRP     EscortTransport
TUG        Tug
UTILA      CombatUtilityVehicle
BARGE      HeavyLifter
FREIGHT    BulkFreighter
FRTB       CargoFerryTFTC-C
FRTA       ModularConveyorTFTC-C
FRTC       ContainerTransport
LTFRTA     MuurianTransportTFTC-C
CORTN      CorellianTransport2
CORV       Corvette2TFTC-C
CORVTA     ModCorvetteTFTC-C
NEB        Frigate2CarrierTFTC-C
FRIGA      ModFrigateTFTC-C
PASNGR     PassengerLiner
CRUSA      CarrackCruiserTFTC-C
CRUSB      StrikeCruiserTFTC-C
ESCRT      EscortCarrierTFTC-C
DREAD      Dreadnaught2TFTC-C
CAL        CalamariCruiserNewTFTC-C
LTCAL      CalamariLightCruiserTFTC-C
INTERD     Interdictor2TFTC-C
VSD        VictoryStarDestroyerTFTC-C
ISD        ImperialStarDestroyerTFTC-C
PLATA      Platform1TFTC-C
PLATB      Platform2TFTC-C
PLATC      Platform3TFTC-C
PLATAB     Platform4TFTC-C
PLATBA     Platform5TFTC-C
PLATCA     Platform6TFTC-C
"

# FlightModels and Materials names differ in case from the list.
find_ci() {
    find "$1" -maxdepth 1 -type f -iname "$2" -print -quit 2>/dev/null
}

copied=0
missing=0
while read -r tie source; do
    [ -n "$tie" ] || continue
    opt="$(find_ci "$models" "$source.opt")"
    if [ -z "$opt" ]; then
        echo "missing: $source.opt (for $tie)" >&2
        missing=$((missing + 1))
        continue
    fi
    cp "$opt" "$out/$tie.opt"
    mat="$(find_ci "$materials" "$source.mat")"
    if [ -n "$mat" ]; then
        cp "$mat" "$out/$tie.mat"
    else
        rm -f "$out/$tie.mat"
        echo "note: no $source.mat; $tie uses default materials" >&2
    fi
    echo "$tie <- $(basename "$opt")${mat:+ + $(basename "$mat")}"
    copied=$((copied + 1))
done <<< "$mapping"

echo "copied $copied models to $out ($missing missing)"

# Cockpits, under the names OpenTIE's 3D cockpits use: the OPT, its
# Materials/*.mat, its DynamicCockpit/*.dc display layout and the craft's .ini.
cockpits="$(dirname "$out")/cockpit3d"
mkdir -p "$cockpits"
for name in TieFighterCockpit TieInterceptorCockpit TieBomberCockpit TieAdvancedX1Cockpit TieDefenderCockpit \
    AssaultGunboatCockpit MissileBoatCockpit; do
    opt="$(find_ci "$models" "$name.opt")"
    if [ -z "$opt" ]; then
        echo "missing: $name.opt" >&2
        continue
    fi
    cp "$opt" "$cockpits/$name.opt"
    mat="$(find_ci "$materials" "$name.mat")"
    [ -n "$mat" ] && cp "$mat" "$cockpits/$name.mat"
    dc="$(find_ci "$tftc/DynamicCockpit" "$name.dc")"
    [ -n "$dc" ] && cp "$dc" "$cockpits/$name.dc"
    # The craft's FlightModels/<Craft>.ini holds its cockpit eye position ([CockpitPov]).
    ini="$(find_ci "$models" "${name%Cockpit}.ini")"
    [ -n "$ini" ] && cp "$ini" "$cockpits/$name.ini"
    echo "$name <- $(basename "$opt")${mat:+ + $(basename "$mat")}${dc:+ + $(basename "$dc")}${ini:+ + $(basename "$ini")}"
done

# The digital iMUSE soundtrack (Music/*.IMC), played with the TFTC music source.
if [ -d "$tftc/Music" ]; then
    mkdir -p "$out/music"
    cp "$tftc"/Music/*.IMC "$out/music/" 2>/dev/null || cp "$tftc"/Music/*.imc "$out/music/"
    echo "copied $(ls "$out/music" | wc -l) music files to $out/music"
fi

# Per-mission skyboxes: each TIE mission's TFTC ini (Missions/1b<B>m<M>tftc.ini)
# names an Effects/CubeMaps cube in its [SkyBoxes] section. The section goes to
# skyboxes/b<B>m<M>.ini and the cube's six faces to skyboxes/cubemaps/<Name>/,
# scaled to 2048 px JPGs when ffmpeg is available (the originals are 4096 px).
if [ -d "$tftc/Missions" ] && [ -d "$tftc/Effects/CubeMaps" ]; then
    skyboxes="$out/skyboxes"
    mkdir -p "$skyboxes/cubemaps"
    have_ffmpeg=0
    command -v ffmpeg > /dev/null 2>&1 && have_ffmpeg=1
    missions=0
    cubes=0
    for ini in "$tftc"/Missions/*; do
        base="$(basename "$ini" | tr 'A-Z' 'a-z')"
        [[ "$base" =~ ^1(b[0-9]+m[0-9]+)tftc\.ini$ ]] || continue
        key="${BASH_REMATCH[1]}"
        section="$(awk '/^\[/{s=tolower($0) ~ /^\[skyboxes\]/; next} s && /^[A-Za-z]/' "$ini" | tr -d '\r')"
        [ -n "$section" ] || continue
        cube="$(printf '%s\n' "$section" | awk -F'=' '{k=$1; gsub(/[ \t]/,"",k)} k=="AllRegions"||k=="Region0"{v=$2; gsub(/^[ \t]+|[ \t]+$/,"",v); print v; exit}')"
        name="$(printf '%s' "$cube" | tr '\\' '/')"
        name="${name##*/}"
        [ -n "$name" ] || continue
        src="$(find "$tftc/Effects/CubeMaps" -maxdepth 1 -type d -iname "$name" -print -quit)"
        if [ -z "$src" ]; then
            echo "missing: cubemap $name (for $key)" >&2
            continue
        fi
        printf '[SkyBoxes]\n%s\n' "$section" > "$skyboxes/$key.ini"
        missions=$((missions + 1))
        dest="$skyboxes/cubemaps/$name"
        [ -d "$dest" ] && continue
        mkdir -p "$dest"
        # Faces end in .<face>.jpg or .png; their stem need not match the folder.
        for face in 0 1 2 3 4 5; do
            file="$(find "$src" -maxdepth 1 -type f \( -iname "*.$face.jpg" -o -iname "*.$face.png" \) -print -quit)"
            if [ -z "$file" ]; then
                echo "missing: $name face $face" >&2
                continue
            fi
            if [ "$have_ffmpeg" = 1 ]; then
                ffmpeg -loglevel error -y -i "$file" -vf "scale='min(2048,iw)':-1" -q:v 2 "$dest/$name.$face.jpg"
            else
                cp "$file" "$dest/$name.$face.${file##*.}"
            fi
        done
        cubes=$((cubes + 1))
    done
    echo "copied skyboxes for $missions missions ($cubes cubemaps) to $skyboxes"
fi
