# FSR 4.0 SDK Configuration
set(FFX_SDK_ROOT "${CMAKE_SOURCE_DIR}/../FidelityFX-SDK")

target_include_directories(
  ${PROJECT_NAME}
  PRIVATE
  "${FFX_SDK_ROOT}/Kits/FidelityFX/api/include"
  "${FFX_SDK_ROOT}/Kits/FidelityFX/framegeneration/include"
  "${FFX_SDK_ROOT}/Kits/FidelityFX/upscalers/include"
  "${FFX_SDK_ROOT}/Kits/FidelityFX/backend/dx12"
  "${FFX_SDK_ROOT}/Kits/OpenSource"  # Anti-Lag 2.0 SDK
)

target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE
  "${FFX_SDK_ROOT}/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.lib"
)

add_definitions(-DFFX_API_DX12)

