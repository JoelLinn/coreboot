#!/usr/bin/env sh
#
# SPDX-License-Identifier: GPL-2.0-only

DATE=""
GITREV=""
TIMESOURCE=""
XGCCPATH="${XGCCPATH:-util/crossgcc/xgcc/bin/}"
MAJOR_VER="0"
MINOR_VER="0"
COREBOOT_VERSION_FILE=".coreboot-version"

export LANG=C
export LC_ALL=C
export TZ=UTC0

XCOMPILE=$1

if [ -z "${XCOMPILE}" ] || [ "$1" = "--help" ]; then
	echo "usage: $0 <xcompile>" >&2
	exit 1
fi

# $1: format string
get_git_head_data() {
	LANG="" git log --no-show-signature -1 --format="format:$1" 2>/dev/null || \
	LANG="" git log -1 --format="format:$1"
}

if [ "${BUILD_TIMELESS}" = "1" ]; then
	GITREV=Timeless
	TIMESOURCE="fixed"
	DATE=0
elif [ "$(git rev-parse --is-inside-work-tree 2>/dev/null)" = "true" ]; then
	GITREV="$(get_git_head_data %h)"
	TIMESOURCE=git
	DATE="$(get_git_head_data %ct)"
	VERSION="$(git describe)"
	MAJOR_VER="$(echo "${VERSION}" | sed 's/\([0-9]\)\.\([0-9][0-9]*\).*/\1/')"
	MINOR_VER="$(echo "${VERSION}" | sed 's/\([0-9]\)\.\([0-9][0-9]*\).*/\2/')"
else
	GITREV=Unknown
	TIMESOURCE="date"
	DATE=$(LANG="" LC_ALL=C TZ=UTC0 date +%s)
	if [ -f "${COREBOOT_VERSION_FILE}" ]; then
		MAJOR_VER="$(sed 's/\([0-9]\)\.\([0-9][0-9]*\).*/\1/' "${COREBOOT_VERSION_FILE}")"
		MINOR_VER="$(sed 's/\([0-9]\)\.\([0-9][0-9]*\).*/\2/' "${COREBOOT_VERSION_FILE}")"
	fi
fi

our_date() {
case $(uname) in
NetBSD|OpenBSD|DragonFly|FreeBSD|Darwin)
	date -r "$1" "$2"
	;;
*)
	date -d "@$1" "$2"
esac
}

# Look for IASL in XGCCPATH and xcompile.  Unfortunately,
# xcompile isn't available on the first build.
# If neither of those gives a valid iasl, check the path.
IASL="${XGCCPATH}iasl"
eval "$(grep ^IASL:= "${XCOMPILE}" 2>/dev/null | sed s,:=,=,)"
if [ ! -x "${IASL}" ]; then
	IASL=$(command -v iasl)
fi
IASLVERSION="$(${IASL} -v | grep version | sed 's/.*version //')" >/dev/null

#Print out the information that goes into build.h
printf "/* build system definitions (autogenerated) */\n"
printf "#ifndef __BUILD_H\n"
printf "#define __BUILD_H\n\n"
printf "#define COREBOOT_VERSION %s\n" "\"${KERNELVERSION}\""

#See if the build is running in a git repo and the git command is available
printf "/* timesource: %s */\n" "${TIMESOURCE}"
printf "#define COREBOOT_VERSION_TIMESTAMP %s\n" "${DATE}"
printf "#define COREBOOT_ORIGIN_GIT_REVISION \"%s\"\n" "${GITREV}"

printf "#define COREBOOT_EXTRA_VERSION \"%s\"\n" "${COREBOOT_EXTRA_VERSION}"
printf "#define COREBOOT_MAJOR_VERSION %s\n" "${MAJOR_VER}"
printf "#define COREBOOT_MINOR_VERSION %s\n" "${MINOR_VER}"
printf "#define COREBOOT_BUILD \"%s\"\n" "$(our_date "${DATE}" "+%a %b %d %H:%M:%S %Z %Y")"
printf "#define COREBOOT_BUILD_YEAR_BCD 0x%s\n" "$(our_date "${DATE}" "+%y")"
printf "#define COREBOOT_BUILD_MONTH_BCD 0x%s\n" "$(our_date "${DATE}" "+%m")"
printf "#define COREBOOT_BUILD_DAY_BCD 0x%s\n" "$(our_date "${DATE}" "+%d")"
printf "#define COREBOOT_BUILD_WEEKDAY_BCD 0x%s\n" "$(our_date "${DATE}" "+%w")"
printf "#define COREBOOT_BUILD_EPOCH \"%s\"\n" "$(our_date "${DATE}" "+%s")"
printf "#define COREBOOT_DMI_DATE \"%s\"\n" "$(our_date "${DATE}" "+%m/%d/%Y")"
printf "\n"
printf "#define COREBOOT_COMPILE_TIME \"%s\"\n" "$(our_date "${DATE}" "+%T")"
printf "#define ASL_VERSION 0x%s\n" "${IASLVERSION}"
printf "#endif\n"
