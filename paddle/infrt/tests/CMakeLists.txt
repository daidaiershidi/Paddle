configure_file(lit.cfg.py.in "${CMAKE_SOURCE_DIR}/paddle/infrt/tests/lit.cfg.py")

add_test(NAME test_infrt_by_lit COMMAND sh -c "lit -v ${CMAKE_SOURCE_DIR}/paddle/infrt/tests --filter-out \"disabled_*\""
    DEPENDS infrtopt infrtexec)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/dialect/tensor/tensor_map.mlir.in ${CMAKE_CURRENT_SOURCE_DIR}/dialect/tensor/tensor_map.mlir)
