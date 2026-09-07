# Former build options remain accepted for existing presets (2026-09-07).
function(unendlich_option suffix description default_value)
  if(NOT DEFINED UNENDLICH_${suffix} AND DEFINED INFINITY_${suffix})
    set(UNENDLICH_${suffix} "${INFINITY_${suffix}}" CACHE BOOL "${description}")
  endif()
  option(UNENDLICH_${suffix} "${description}" ${default_value})
endfunction()
