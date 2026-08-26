#!/usr/bin/env bash

# Shared local build environment for CMake, Gradle externalNativeBuild and adb.
# Source this file from a shell before running CMake directly:
#   source scaffold/env.sh

_gis_md_env_is_sourced=0
if [ -n "${BASH_VERSION:-}" ] && [ -n "${BASH_SOURCE[0]:-}" ]; then
    _gis_md_env_script="${BASH_SOURCE[0]}"
    if [ "$_gis_md_env_script" != "$0" ]; then
        _gis_md_env_is_sourced=1
    fi
else
    _gis_md_env_script="$0"
    if [ -n "${ZSH_VERSION:-}" ]; then
        case "${ZSH_EVAL_CONTEXT:-}" in
            *:file*) _gis_md_env_is_sourced=1 ;;
        esac
    fi
fi

export GIS_MD_SCAFFOLD_DIR="${GIS_MD_SCAFFOLD_DIR:-$(cd "$(dirname "$_gis_md_env_script")" && pwd)}"

export ANDROID_HOME="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$ANDROID_HOME}"
export ANDROID_NDK_VERSION="${ANDROID_NDK_VERSION:-28.2.13676358}"
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/$ANDROID_NDK_VERSION}"
export ANDROID_CMAKE_VERSION="${ANDROID_CMAKE_VERSION:-3.22.1}"
export ANDROID_CMAKE_HOME="${ANDROID_CMAKE_HOME:-$ANDROID_HOME/cmake/$ANDROID_CMAKE_VERSION}"

if [ -z "${JAVA_HOME:-}" ]; then
    for _gis_md_java_candidate in \
        "$HOME/development/jdks/zulu17.64.17-ca-jdk17.0.18-macosx_aarch64/zulu-17.jdk/Contents/Home" \
        "$HOME/sdk/jdk-17"; do
        if [ -d "$_gis_md_java_candidate" ]; then
            export JAVA_HOME="$_gis_md_java_candidate"
            break
        fi
    done
fi

if [ -z "${JAVA_HOME:-}" ] && [ "$(uname -s 2>/dev/null || true)" = "Darwin" ] && \
   [ -x /usr/libexec/java_home ]; then
    _gis_md_java_home_from_system="$(/usr/libexec/java_home -v 17 2>/dev/null || true)"
    if [ -n "$_gis_md_java_home_from_system" ] && [ -d "$_gis_md_java_home_from_system" ]; then
        export JAVA_HOME="$_gis_md_java_home_from_system"
    fi
fi

_gis_md_prepend_path() {
    if [ -d "$1" ]; then
        case ":$PATH:" in
            *":$1:"*) ;;
            *) export PATH="$1:$PATH" ;;
        esac
    fi
}

_gis_md_prepend_path "$ANDROID_CMAKE_HOME/bin"
_gis_md_prepend_path "$ANDROID_HOME/platform-tools"
_gis_md_prepend_path "$ANDROID_HOME/cmdline-tools/latest/bin"
if [ -n "${JAVA_HOME:-}" ]; then
    _gis_md_prepend_path "$JAVA_HOME/bin"
fi

if [ -z "${PKG_CONFIG:-}" ] && \
   [ -x "$GIS_MD_SCAFFOLD_DIR/third_party/pkgconf/bin/pkg-config" ]; then
    export PKG_CONFIG="$GIS_MD_SCAFFOLD_DIR/third_party/pkgconf/bin/pkg-config"
fi

if [ -z "${VCPKG_ROOT:-}" ]; then
    for _gis_md_vcpkg_root in \
        "$GIS_MD_SCAFFOLD_DIR/third_party/vcpkg" \
        "$GIS_MD_SCAFFOLD_DIR/../../globe/third_party/vcpkg" \
        "$GIS_MD_SCAFFOLD_DIR/../../cesium-native/extern/vcpkg"; do
        if [ -x "$_gis_md_vcpkg_root/vcpkg" ]; then
            export VCPKG_ROOT="$_gis_md_vcpkg_root"
            break
        fi
    done
fi

if [ -z "${GLM_INCLUDE_DIR:-}" ]; then
    for _gis_md_glm_include in \
        "$GIS_MD_SCAFFOLD_DIR/third_party/glm/include" \
        "${VCPKG_ROOT:-}/installed/${VCPKG_DEFAULT_TRIPLET:-arm64-osx}/include" \
        "${VCPKG_ROOT:-}/packages/glm_arm64-android/include" \
        "$GIS_MD_SCAFFOLD_DIR/../../globe/third_party/vcpkg/packages/glm_arm64-android/include"; do
        if [ -f "$_gis_md_glm_include/glm/glm.hpp" ]; then
            export GLM_INCLUDE_DIR="$_gis_md_glm_include"
            break
        fi
    done
fi

if [ -z "${NLOHMANN_JSON_INCLUDE_DIR:-}" ] && \
   [ -f "$GIS_MD_SCAFFOLD_DIR/third_party_py/nlohmann_json/include/nlohmann/json.hpp" ]; then
    export NLOHMANN_JSON_INCLUDE_DIR="$GIS_MD_SCAFFOLD_DIR/third_party_py/nlohmann_json/include"
fi

unset _gis_md_java_candidate
unset _gis_md_java_home_from_system
unset _gis_md_env_script
unset _gis_md_glm_include
unset _gis_md_vcpkg_root

if [ "$_gis_md_env_is_sourced" -eq 0 ]; then
    echo "Loaded gis-md build environment for child processes."
    echo "For the current shell, run: source $GIS_MD_SCAFFOLD_DIR/env.sh"
    echo "cmake: $(command -v cmake || echo not found)"
    echo "ninja: $(command -v ninja || echo not found)"
    echo "ANDROID_NDK_HOME: ${ANDROID_NDK_HOME:-not set}"
    echo "GLM_INCLUDE_DIR: ${GLM_INCLUDE_DIR:-not set}"
fi

unset _gis_md_env_is_sourced
