ycxxc_unit_test(pp pp_test.cpp ycxxc_pp)
include(GNUInstallDirs)
add_test(NAME unit.pp_relocated_prefix
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/relocated_prefix.py
          $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/resources "${CMAKE_INSTALL_DATADIR}")
