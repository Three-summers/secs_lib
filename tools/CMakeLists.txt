add_executable(secs-sml-check
  secs_sml_check.cpp
)

target_link_libraries(secs-sml-check PRIVATE secs_sml)
target_link_libraries(secs-sml-check PRIVATE secs_tools)

add_executable(secs-recorder
  secs_recorder.cpp
)
target_link_libraries(secs-recorder PRIVATE secs_hsms secs_tools)

add_executable(secs-player
  secs_player.cpp
)
target_link_libraries(secs-player PRIVATE secs_hsms secs_tools)

add_executable(secs-hsms-bulk
  secs_hsms_bulk.cpp
)
target_link_libraries(secs-hsms-bulk PRIVATE secs_hsms secs_utils)

add_executable(secs-hsms-probe
  secs_hsms_probe.cpp
)
target_link_libraries(secs-hsms-probe PRIVATE secs_protocol secs_tools secs_utils)

if(TARGET secs_rpc)
  add_executable(secs-rpc-server
    secs_rpc_server.cpp
  )
  target_link_libraries(secs-rpc-server PRIVATE secs_rpc)
endif()

if(SECS_ENABLE_INSTALL)
  install(TARGETS secs-sml-check
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
  install(TARGETS secs-recorder secs-player
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
  install(TARGETS secs-hsms-bulk
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
  install(TARGETS secs-hsms-probe
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
  if(TARGET secs-rpc-server)
    install(TARGETS secs-rpc-server
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
  endif()
endif()
