#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

find_ndk_glslc() {
  local ndk_root=""
  local sdk_root=""
  local shader_tools_dir=""
  local host_tag=""
  local candidate=""

  ndk_root="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-${ANDROID_NDK:-}}}"
  if [[ -z "$ndk_root" ]]; then
    sdk_root="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
    if [[ -n "$sdk_root" ]]; then
      if [[ -d "$sdk_root/ndk" ]]; then
        while IFS= read -r candidate; do
          if [[ -d "$candidate/shader-tools" ]]; then
            ndk_root="$candidate"
            break
          fi
        done < <(find "$sdk_root/ndk" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -r)
      fi

      if [[ -z "$ndk_root" && -d "$sdk_root/ndk-bundle/shader-tools" ]]; then
        ndk_root="$sdk_root/ndk-bundle"
      fi
    fi
  fi

  if [[ -z "$ndk_root" ]]; then
    return 1
  fi

  shader_tools_dir="$ndk_root/shader-tools"
  if [[ ! -d "$shader_tools_dir" ]]; then
    return 1
  fi

  case "$(uname -s)" in
    Darwin)
      host_tag="darwin-x86_64"
      ;;
    Linux)
      host_tag="linux-x86_64"
      ;;
    MINGW*|MSYS*|CYGWIN*)
      host_tag="windows-x86_64"
      ;;
  esac

  if [[ -n "$host_tag" ]]; then
    candidate="$shader_tools_dir/$host_tag/glslc"
    if [[ -x "$candidate" ]]; then
      echo "$candidate"
      return 0
    fi

    candidate="$shader_tools_dir/$host_tag/glslc.exe"
    if [[ -x "$candidate" ]]; then
      echo "$candidate"
      return 0
    fi
  fi

  while IFS= read -r candidate; do
    if [[ -x "$candidate" ]]; then
      echo "$candidate"
      return 0
    fi
  done < <(find "$shader_tools_dir" -maxdepth 2 -type f \( -name glslc -o -name glslc.exe \) 2>/dev/null | sort)

  return 1
}

if command -v glslc >/dev/null 2>&1; then
  SHADER_COMPILER=(glslc)
else
  NDK_GLSLC="$(find_ndk_glslc || true)"
  if [[ -n "$NDK_GLSLC" ]]; then
    SHADER_COMPILER=("$NDK_GLSLC")
  elif command -v glslangValidator >/dev/null 2>&1; then
    SHADER_COMPILER=(glslangValidator)
  else
    echo "No shader compiler found. Install 'glslc' or 'glslangValidator'." >&2
    echo "Tip: set ANDROID_NDK_HOME to an NDK containing shader-tools/glslc." >&2
    exit 1
  fi
fi

if ! command -v xxd >/dev/null 2>&1; then
  echo "Missing required tool: xxd" >&2
  exit 1
fi

MODE="write"
if [[ "${1:-}" == "--check" ]]; then
  MODE="check"
fi

compile_shader() {
  local source="$1"
  local stage="$2"
  local output="$3"
  shift 3
  if [[ "${SHADER_COMPILER[0]##*/}" == glslc* ]]; then
    "${SHADER_COMPILER[@]}" -fshader-stage="$stage" "$@" -o "$output" "$source"
  else
    "${SHADER_COMPILER[@]}" -V -S "$stage" "$@" -o "$output" "$source"
  fi
}

generate_header() {
  local source="$1"
  local stage="$2"
  local symbol_name="$3"
  local output_header="$4"
  shift 4

  local tmp_spv
  local tmp_header
  tmp_spv="$(mktemp)"
  tmp_header="$(mktemp)"

  compile_shader "$source" "$stage" "$tmp_spv" "$@"

  {
    echo "#pragma once"
    echo "#include <cstddef>"
    xxd -i -n "$symbol_name" "$tmp_spv"
  } > "$tmp_header"

  if [[ "$MODE" == "check" ]]; then
    if ! cmp -s "$tmp_header" "$output_header"; then
      echo "Outdated shader header: $output_header" >&2
      rm -f "$tmp_spv" "$tmp_header"
      return 1
    fi
  else
    if [[ -f "$output_header" ]] && cmp -s "$tmp_header" "$output_header"; then
      echo "Unchanged $output_header"
    else
      mv "$tmp_header" "$output_header"
      echo "Updated $output_header"
    fi
  fi

  rm -f "$tmp_spv" "$tmp_header"
}

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_InterpSpansShader.comp" \
  "comp" \
  "melonDS_gpu3d_vulkan_interp_spans_comp_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_InterpSpansShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_BinCombinedShader.comp" \
  "comp" \
  "melonDS_gpu3d_vulkan_bin_combined_comp_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_BinCombinedShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_CalculateWorkOffsetsShader.comp" \
  "comp" \
  "melonDS_gpu3d_vulkan_calc_work_offsets_comp_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_CalculateWorkOffsetsShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_SortWorkShader.comp" \
  "comp" \
  "melonDS_gpu3d_vulkan_sort_work_comp_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_SortWorkShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_TriRasterShader.comp" \
  "comp" \
  "melonDS_gpu3d_vulkan_tri_raster_comp_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_TriRasterShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_TriRasterBaseShader.comp" \
  "comp" \
  "melonDS_gpu3d_vulkan_tri_raster_base_comp_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_TriRasterBaseShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_TriRasterCompatShader.comp" \
  "comp" \
  "melonDS_gpu3d_vulkan_tri_raster_compat_comp_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_TriRasterCompatShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_DepthBlendShader.comp" \
  "comp" \
  "melonDS_gpu3d_vulkan_depth_blend_comp_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_DepthBlendShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_FinalPassShader.comp" \
  "comp" \
  "melonDS_gpu3d_vulkan_final_pass_comp_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_FinalPassShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_CaptureLineExportShader.comp" \
  "comp" \
  "melonDS_gpu3d_vulkan_capture_line_export_comp_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_CaptureLineExportShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterShader.vert" \
  "vert" \
  "melonDS_gpu3d_vulkan_graphics_raster_vert_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterShaderVertexData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_gpu3d_vulkan_graphics_raster_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterShaderFragmentData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_gpu3d_vulkan_graphics_raster_no_frag_depth_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthShaderFragmentData.h" \
  "-DMELONDS_NO_FRAG_DEPTH=1"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_gpu3d_vulkan_graphics_raster_no_frag_depth_direct_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectShaderFragmentData.h" \
  "-DMELONDS_NO_FRAG_DEPTH=1" \
  "-DMELONDS_DIRECT_TEXTURE_INDEXING=1"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_gpu3d_vulkan_graphics_raster_no_frag_depth_direct_fast_modulate_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateShaderFragmentData.h" \
  "-DMELONDS_NO_FRAG_DEPTH=1" \
  "-DMELONDS_DIRECT_TEXTURE_INDEXING=1" \
  "-DMELONDS_FAST_OPAQUE_MODULATE=1" \
  "-DMELONDS_FAST_TEXTURE_PUSH_CONSTANTS=1"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_gpu3d_vulkan_graphics_raster_no_frag_depth_direct_fast_modulate_toon_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateToonShaderFragmentData.h" \
  "-DMELONDS_NO_FRAG_DEPTH=1" \
  "-DMELONDS_DIRECT_TEXTURE_INDEXING=1" \
  "-DMELONDS_FAST_OPAQUE_MODULATE=1" \
  "-DMELONDS_FAST_TOON_MODE=1" \
  "-DMELONDS_FAST_TEXTURE_PUSH_CONSTANTS=1"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_gpu3d_vulkan_graphics_raster_no_frag_depth_direct_fast_modulate_plain_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulatePlainShaderFragmentData.h" \
  "-DMELONDS_NO_FRAG_DEPTH=1" \
  "-DMELONDS_DIRECT_TEXTURE_INDEXING=1" \
  "-DMELONDS_FAST_OPAQUE_MODULATE=1" \
  "-DMELONDS_FAST_TOON_MODE=2" \
  "-DMELONDS_FAST_TEXTURE_PUSH_CONSTANTS=1"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_gpu3d_vulkan_graphics_raster_no_frag_depth_direct_fast_modulate_opaque_alpha_toon_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateOpaqueAlphaToonShaderFragmentData.h" \
  "-DMELONDS_NO_FRAG_DEPTH=1" \
  "-DMELONDS_DIRECT_TEXTURE_INDEXING=1" \
  "-DMELONDS_FAST_OPAQUE_MODULATE=1" \
  "-DMELONDS_FAST_TOON_MODE=1" \
  "-DMELONDS_FAST_TEXTURE_PUSH_CONSTANTS=1" \
  "-DMELONDS_FAST_OPAQUE_FULL_ALPHA=1"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_gpu3d_vulkan_graphics_raster_no_frag_depth_direct_fast_modulate_opaque_alpha_plain_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateOpaqueAlphaPlainShaderFragmentData.h" \
  "-DMELONDS_NO_FRAG_DEPTH=1" \
  "-DMELONDS_DIRECT_TEXTURE_INDEXING=1" \
  "-DMELONDS_FAST_OPAQUE_MODULATE=1" \
  "-DMELONDS_FAST_TOON_MODE=2" \
  "-DMELONDS_FAST_TEXTURE_PUSH_CONSTANTS=1" \
  "-DMELONDS_FAST_OPAQUE_FULL_ALPHA=1" \
  "-DMELONDS_FAST_FLOAT_MODULATE=1"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_gpu3d_vulkan_graphics_raster_no_frag_depth_direct_fast_modulate_opaque_alpha_plain_no_attr_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateOpaqueAlphaPlainNoAttrShaderFragmentData.h" \
  "-DMELONDS_NO_FRAG_DEPTH=1" \
  "-DMELONDS_DIRECT_TEXTURE_INDEXING=1" \
  "-DMELONDS_FAST_OPAQUE_MODULATE=1" \
  "-DMELONDS_FAST_TOON_MODE=2" \
  "-DMELONDS_FAST_TEXTURE_PUSH_CONSTANTS=1" \
  "-DMELONDS_FAST_OPAQUE_FULL_ALPHA=1" \
  "-DMELONDS_FAST_FLOAT_MODULATE=1" \
  "-DMELONDS_COLOR_ONLY=1"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_gpu3d_vulkan_graphics_raster_no_frag_depth_direct_fast_modulate_opaque_alpha_plain_color_only_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateOpaqueAlphaPlainColorOnlyShaderFragmentData.h" \
  "-DMELONDS_NO_FRAG_DEPTH=1" \
  "-DMELONDS_DIRECT_TEXTURE_INDEXING=1" \
  "-DMELONDS_FAST_OPAQUE_MODULATE=1" \
  "-DMELONDS_FAST_TOON_MODE=2" \
  "-DMELONDS_FAST_TEXTURE_PUSH_CONSTANTS=1" \
  "-DMELONDS_FAST_OPAQUE_FULL_ALPHA=1" \
  "-DMELONDS_COLOR_ONLY=1"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsNoColorShader.frag" \
  "frag" \
  "melonDS_gpu3d_vulkan_graphics_no_color_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsNoColorShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsClearShader.frag" \
  "frag" \
  "melonDS_gpu3d_vulkan_graphics_clear_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsClearShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsFinalShader.vert" \
  "vert" \
  "melonDS_gpu3d_vulkan_graphics_final_vert_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsFinalShaderVertexData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsEdgeShader.frag" \
  "frag" \
  "melonDS_gpu3d_vulkan_graphics_edge_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsEdgeShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsEdgeFogShader.frag" \
  "frag" \
  "melonDS_gpu3d_vulkan_graphics_edge_fog_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsEdgeFogShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsFogShader.frag" \
  "frag" \
  "melonDS_gpu3d_vulkan_graphics_fog_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/GPU3D_Vulkan_GraphicsFogShaderData.h"

# Compatibility profile shaders are compiled from the exact pre-fastpath
# sources at melonDS-android-lib commit 39363b7c. They deliberately use
# distinct symbols so one native engine can keep both Vulkan pipeline
# strategies resident without duplicating the whole core.
generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_TriRasterBaseShader.comp" \
  "comp" \
  "melonDS_compat_gpu3d_vulkan_tri_raster_base_comp_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_TriRasterBaseShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_CaptureLineExportShader.comp" \
  "comp" \
  "melonDS_compat_gpu3d_vulkan_capture_line_export_comp_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_CaptureLineExportShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_compat_gpu3d_vulkan_graphics_raster_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsRasterShaderFragmentData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_compat_gpu3d_vulkan_graphics_raster_no_frag_depth_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsRasterNoFragDepthShaderFragmentData.h" \
  "-DMELONDS_NO_FRAG_DEPTH=1"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_compat_gpu3d_vulkan_graphics_raster_no_frag_depth_direct_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectShaderFragmentData.h" \
  "-DMELONDS_NO_FRAG_DEPTH=1" \
  "-DMELONDS_DIRECT_TEXTURE_INDEXING=1"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_compat_gpu3d_vulkan_graphics_raster_no_frag_depth_direct_fast_modulate_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateShaderFragmentData.h" \
  "-DMELONDS_NO_FRAG_DEPTH=1" \
  "-DMELONDS_DIRECT_TEXTURE_INDEXING=1" \
  "-DMELONDS_FAST_OPAQUE_MODULATE=1" \
  "-DMELONDS_FAST_TEXTURE_PUSH_CONSTANTS=1"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_compat_gpu3d_vulkan_graphics_raster_no_frag_depth_direct_fast_modulate_toon_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateToonShaderFragmentData.h" \
  "-DMELONDS_NO_FRAG_DEPTH=1" \
  "-DMELONDS_DIRECT_TEXTURE_INDEXING=1" \
  "-DMELONDS_FAST_OPAQUE_MODULATE=1" \
  "-DMELONDS_FAST_TOON_MODE=1" \
  "-DMELONDS_FAST_TEXTURE_PUSH_CONSTANTS=1"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_compat_gpu3d_vulkan_graphics_raster_no_frag_depth_direct_fast_modulate_plain_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulatePlainShaderFragmentData.h" \
  "-DMELONDS_NO_FRAG_DEPTH=1" \
  "-DMELONDS_DIRECT_TEXTURE_INDEXING=1" \
  "-DMELONDS_FAST_OPAQUE_MODULATE=1" \
  "-DMELONDS_FAST_TOON_MODE=2" \
  "-DMELONDS_FAST_TEXTURE_PUSH_CONSTANTS=1"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_compat_gpu3d_vulkan_graphics_raster_no_frag_depth_direct_fast_modulate_opaque_alpha_toon_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateOpaqueAlphaToonShaderFragmentData.h" \
  "-DMELONDS_NO_FRAG_DEPTH=1" \
  "-DMELONDS_DIRECT_TEXTURE_INDEXING=1" \
  "-DMELONDS_FAST_OPAQUE_MODULATE=1" \
  "-DMELONDS_FAST_TOON_MODE=1" \
  "-DMELONDS_FAST_TEXTURE_PUSH_CONSTANTS=1" \
  "-DMELONDS_FAST_OPAQUE_FULL_ALPHA=1"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsRasterShader.frag" \
  "frag" \
  "melonDS_compat_gpu3d_vulkan_graphics_raster_no_frag_depth_direct_fast_modulate_opaque_alpha_plain_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateOpaqueAlphaPlainShaderFragmentData.h" \
  "-DMELONDS_NO_FRAG_DEPTH=1" \
  "-DMELONDS_DIRECT_TEXTURE_INDEXING=1" \
  "-DMELONDS_FAST_OPAQUE_MODULATE=1" \
  "-DMELONDS_FAST_TOON_MODE=2" \
  "-DMELONDS_FAST_TEXTURE_PUSH_CONSTANTS=1" \
  "-DMELONDS_FAST_OPAQUE_FULL_ALPHA=1"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsNoColorShader.frag" \
  "frag" \
  "melonDS_compat_gpu3d_vulkan_graphics_no_color_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsNoColorShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsEdgeShader.frag" \
  "frag" \
  "melonDS_compat_gpu3d_vulkan_graphics_edge_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsEdgeShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsEdgeFogShader.frag" \
  "frag" \
  "melonDS_compat_gpu3d_vulkan_graphics_edge_fog_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsEdgeFogShaderData.h"

generate_header \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsFogShader.frag" \
  "frag" \
  "melonDS_compat_gpu3d_vulkan_graphics_fog_frag_spv" \
  "$ROOT_DIR/melonDS-android-lib/src/compatibility/GPU3D_Vulkan_GraphicsFogShaderData.h"

generate_header \
  "$ROOT_DIR/app/src/main/cpp/renderer/VulkanCompositorShader.comp" \
  "comp" \
  "melonDS_android_vulkan_compositor_comp_spv" \
  "$ROOT_DIR/app/src/main/cpp/renderer/VulkanCompositorShaderData.h"

generate_header \
  "$ROOT_DIR/app/src/main/cpp/renderer/VulkanCompositorCompatibilityShader.comp" \
  "comp" \
  "melonDS_android_vulkan_compositor_compatibility_comp_spv" \
  "$ROOT_DIR/app/src/main/cpp/renderer/VulkanCompositorCompatibilityShaderData.h"

generate_header \
  "$ROOT_DIR/app/src/main/cpp/renderer/VulkanAccumulate3dShader.comp" \
  "comp" \
  "melonDS_android_vulkan_accumulate_3d_comp_spv" \
  "$ROOT_DIR/app/src/main/cpp/renderer/VulkanAccumulate3dShaderData.h"

generate_header \
  "$ROOT_DIR/app/src/main/cpp/renderer/VulkanAccumulate3dCompatibilityShader.comp" \
  "comp" \
  "melonDS_android_vulkan_accumulate_3d_compatibility_comp_spv" \
  "$ROOT_DIR/app/src/main/cpp/renderer/VulkanAccumulate3dCompatibilityShaderData.h"

generate_header \
  "$ROOT_DIR/app/src/main/cpp/renderer/VulkanAccumulate3dScale8Shader.comp" \
  "comp" \
  "melonDS_android_vulkan_accumulate_3d_scale8_comp_spv" \
  "$ROOT_DIR/app/src/main/cpp/renderer/VulkanAccumulate3dScale8ShaderData.h"

generate_header \
  "$ROOT_DIR/app/src/main/cpp/renderer/VulkanSurfacePresenter.vert" \
  "vert" \
  "melonDS_android_vulkan_surface_presenter_vert_spv" \
  "$ROOT_DIR/app/src/main/cpp/renderer/VulkanSurfacePresenterVertexShaderData.h"

generate_header \
  "$ROOT_DIR/app/src/main/cpp/renderer/VulkanSurfacePresenter.frag" \
  "frag" \
  "melonDS_android_vulkan_surface_presenter_frag_spv" \
  "$ROOT_DIR/app/src/main/cpp/renderer/VulkanSurfacePresenterFragmentShaderData.h"

generate_header \
  "$ROOT_DIR/app/src/main/cpp/renderer/VulkanSurfacePresenterCompatibility.frag" \
  "frag" \
  "melonDS_android_vulkan_surface_presenter_compatibility_frag_spv" \
  "$ROOT_DIR/app/src/main/cpp/renderer/VulkanSurfacePresenterCompatibilityFragmentShaderData.h"

if [[ "$MODE" == "check" ]]; then
  echo "Vulkan SPIR-V headers are up to date."
fi
