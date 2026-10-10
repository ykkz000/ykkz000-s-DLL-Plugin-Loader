project(YKKZ000Loader LANGUAGES CXX)

add_library(YKKZ000Loader SHARED
  src/dllmain.cpp
  src/proxy.cpp
  src/gamecore.cpp
  src/logging.cpp
  src/registry.cpp
  src/vtable_clone.cpp
  src/effect_mechanism.cpp
  src/hook_service.cpp
  src/effect_handler.cpp
  src/memory_probe.cpp
  src/crash_capture.cpp
  src/serialization_trace.cpp
  src/plugin_manager.cpp
  resource.rc
  loader.def)

set_target_properties(YKKZ000Loader PROPERTIES
  OUTPUT_NAME "GameCore_YKKZ000_Loader_XP2"
  PREFIX "")

target_include_directories(YKKZ000Loader PRIVATE include)

target_link_libraries(YKKZ000Loader PRIVATE ykkz000_bridge minhook::minhook log4cxx user32 advapi32 shell32 ole32)

if(DISABLE_EFFECT_HANDLER)
  target_compile_definitions(YKKZ000Loader PRIVATE DISABLE_EFFECT_HANDLER)
endif()

if(DISABLE_HANDLER_GUARD)
  target_compile_definitions(YKKZ000Loader PRIVATE DISABLE_HANDLER_GUARD)
endif()

if(ENABLE_DISPATCH_TRACE)
  target_compile_definitions(YKKZ000Loader PRIVATE ENABLE_DISPATCH_TRACE)
endif()
