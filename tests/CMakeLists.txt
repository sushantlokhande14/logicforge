add_executable(unit_tests unit_tests.cpp)
target_link_libraries(unit_tests PRIVATE logicforge)
add_test(NAME unit COMMAND unit_tests)

add_executable(equiv_tests equiv_tests.cpp)
target_link_libraries(equiv_tests PRIVATE logicforge)
add_test(NAME equiv COMMAND equiv_tests ${CMAKE_CURRENT_SOURCE_DIR}/designs)

add_executable(qor_regress qor_regress.cpp)
target_link_libraries(qor_regress PRIVATE logicforge)
add_test(NAME qor COMMAND qor_regress ${CMAKE_CURRENT_SOURCE_DIR}/designs ${CMAKE_CURRENT_SOURCE_DIR}/golden/qor.txt)
