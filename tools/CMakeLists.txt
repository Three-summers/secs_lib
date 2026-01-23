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

if(SECS_ENABLE_INSTALL)
  install(TARGETS secs-sml-check
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
  install(TARGETS secs-recorder secs-player
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
endif()
