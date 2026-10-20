set(libevent_FOUND TRUE)
set(LIBEVENT_INCLUDE_DIRS "${DEPS}/libevent/include" "${CMAKE_CURRENT_BINARY_DIR}/deps/libevent/include")
set(LIBEVENT_LIBRARIES event_core_static event_extra_static)
