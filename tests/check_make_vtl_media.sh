#!/bin/bash
#
# Check that make_vtl_media reads every documented barcode suffix as the
# right density. The table is the legend in etc/generate_library_contents.in,
# plus the suffixes set_density() handles that the legend leaves out.
#
# Runs set_density() and set_media_type() straight from
# usr/cmd/make_vtl_media.in, so no build, root or mhvtl install is needed.
#
# Usage: tests/check_make_vtl_media.sh

set -u

here=$(cd "$(dirname "$0")" && pwd)
script="$here/../usr/cmd/make_vtl_media.in"

# Pull the two functions out of the script and define them here.
eval "$(sed -n '/^set_media_type()/,/^}/p; /^set_density()/,/^}/p' "$script")"

failed=0
checked=0

# barcode  density  media type. The loop's names are prefixed because
# set_density() and set_media_type() assign plain globals (density, type).
while read -r want_barcode want_density want_kind; do
	[[ -z "$want_barcode" || "$want_barcode" == \#* ]] && continue
	set_density "$want_barcode"
	set_media_type "$want_barcode"
	checked=$((checked + 1))
	if [[ "$DENSITY" != "$want_density" || "$MEDIA_TYPE" != "$want_kind" ]]; then
		printf 'FAIL %-10s density %-8s (want %-8s) type %-5s (want %s)\n' \
			"$want_barcode" "$DENSITY" "$want_density" "$MEDIA_TYPE" "$want_kind"
		failed=$((failed + 1))
	fi
done <<'TABLE'
# LTO data cartridges
E01001L1  LTO1     data
E01001L2  LTO2     data
E01001L3  LTO3     data
E01001L4  LTO4     data
E01001L5  LTO5     data
E01001L6  LTO6     data
E01001L7  LTO7     data
E01001L8  LTO8     data
E01001L9  LTO9     data
E01001LA  LTO10    data
E01001PA  LTO10P   data
# LTO WORM cartridges (LT used to be UNKNOWN)
E01001LT  LTO3     WORM
E01001LU  LTO4     WORM
E01001LV  LTO5     WORM
E01001LW  LTO6     WORM
E01001LX  LTO7     WORM
E01001LY  LTO8     WORM
E01001LZ  LTO9     WORM
E01001LH  LTO10    WORM
# cleaning cartridges
CLN101L8  LTO8     clean
# DLT / SDLT / AIT
E01001D7  DLT4     data
E01001S1  SDLT220  data
E01001S2  SDLT320  data
E01001S3  SDLT600  data
E01001X1  AIT1     data
E01001X4  AIT4     data
# STK 9840 / 9940 / T10000 (TC used to be UNKNOWN)
E01001TZ  9840A    data
E01001TY  9840B    data
E01001TX  9840C    data
E01001TW  9840D    data
E01001TV  9940A    data
E01001TU  9940B    data
E01001TA  T10KA    data
E01001TB  T10KB    data
E01001TC  T10KC    data
# IBM 3592 (JC used to be UNKNOWN)
E01001JA  J1A      data
E01001JB  E05      data
E01001JC  E06      data
E01001JK  E07      data
E01001JW  E05      WORM
E01001JX  E05      WORM
E01001JY  E07      WORM
# not a media suffix
E01001QQ  UNKNOWN  data
TABLE

echo "make_vtl_media: $checked barcodes checked, $failed failed"
[[ $failed -eq 0 ]]
