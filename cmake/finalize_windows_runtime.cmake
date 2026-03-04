if(NOT DEFINED DEPLOY_DIR OR DEPLOY_DIR STREQUAL "")
  message(FATAL_ERROR "DEPLOY_DIR is required")
endif()

file(TO_CMAKE_PATH "${DEPLOY_DIR}" DEPLOY_DIR)
file(MAKE_DIRECTORY "${DEPLOY_DIR}")

set(REQUIRED_RUNTIME
  msvcp140.dll
  msvcp140_1.dll
  vcruntime140.dll
  vcruntime140_1.dll
  concrt140.dll
)

set(RUNTIME_ROOTS)
if(DEFINED ENV{VCToolsRedistDir} AND NOT "$ENV{VCToolsRedistDir}" STREQUAL "")
  list(APPEND RUNTIME_ROOTS
    "$ENV{VCToolsRedistDir}/x64/Microsoft.VC143.CRT"
    "$ENV{VCToolsRedistDir}/x64/Microsoft.VC142.CRT"
  )
endif()

if(DEFINED ENV{VCToolsInstallDir} AND NOT "$ENV{VCToolsInstallDir}" STREQUAL "")
  file(GLOB _toolchain_roots "$ENV{VCToolsInstallDir}/../../../Redist/MSVC/*/x64/Microsoft.VC*.CRT")
  list(APPEND RUNTIME_ROOTS ${_toolchain_roots})
endif()

if(DEFINED ENV{ProgramFiles(x86)} AND NOT "$ENV{ProgramFiles(x86)}" STREQUAL "")
  file(GLOB _vs_roots "$ENV{ProgramFiles(x86)}/Microsoft Visual Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT")
  list(APPEND RUNTIME_ROOTS ${_vs_roots})
endif()

list(REMOVE_DUPLICATES RUNTIME_ROOTS)
set(VALID_RUNTIME_ROOTS)
foreach(root IN LISTS RUNTIME_ROOTS)
  if(IS_DIRECTORY "${root}")
    list(APPEND VALID_RUNTIME_ROOTS "${root}")
  endif()
endforeach()

# Copy all CRT DLLs we can find from runtime roots.
foreach(root IN LISTS VALID_RUNTIME_ROOTS)
  file(GLOB _crt_dlls "${root}/*.dll")
  foreach(dll IN LISTS _crt_dlls)
    file(COPY "${dll}" DESTINATION "${DEPLOY_DIR}")
  endforeach()
endforeach()

# Ensure mandatory runtime DLLs exist beside executable.
foreach(dll IN LISTS REQUIRED_RUNTIME)
  if(EXISTS "${DEPLOY_DIR}/${dll}")
    continue()
  endif()

  set(found FALSE)
  foreach(root IN LISTS VALID_RUNTIME_ROOTS)
    if(EXISTS "${root}/${dll}")
      file(COPY "${root}/${dll}" DESTINATION "${DEPLOY_DIR}")
      set(found TRUE)
      break()
    endif()
  endforeach()

  if(NOT found AND DEFINED ENV{WINDIR} AND NOT "$ENV{WINDIR}" STREQUAL "" AND EXISTS "$ENV{WINDIR}/System32/${dll}")
    file(COPY "$ENV{WINDIR}/System32/${dll}" DESTINATION "${DEPLOY_DIR}")
    set(found TRUE)
  endif()

  if(NOT found OR NOT EXISTS "${DEPLOY_DIR}/${dll}")
    message(FATAL_ERROR "Missing runtime dependency after deployment: ${dll}")
  endif()
endforeach()

# Strip runtime installer packages; keep DLL-only portable output.
file(GLOB _installers
  "${DEPLOY_DIR}/vc_redist*.exe"
  "${DEPLOY_DIR}/VC_redist*.exe"
  "${DEPLOY_DIR}/vcredist*.exe"
  "${DEPLOY_DIR}/vcredist*.msi"
)
if(_installers)
  file(REMOVE ${_installers})
endif()
