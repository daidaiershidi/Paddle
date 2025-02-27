file(GLOB TEST_OPS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "test_*.py")
string(REPLACE ".py" "" TEST_OPS "${TEST_OPS}")

if ((NOT WITH_GPU) AND (NOT WITH_XPU) AND NOT (WITH_ASCEND OR WITH_ASCEND_CL))
    list(REMOVE_ITEM TEST_OPS "test_dist_fuse_adam_pass")
    list(REMOVE_ITEM TEST_OPS "test_dist_fuse_all_reduce_pass")
    list(REMOVE_ITEM TEST_OPS "test_dist_fuse_bn_act_pass")
    list(REMOVE_ITEM TEST_OPS "test_dist_fuse_bn_add_act_pass")
    list(REMOVE_ITEM TEST_OPS "test_dist_fuse_momentum_pass")
    list(REMOVE_ITEM TEST_OPS "test_dist_fuse_relu_depthwise_conv_pass")
    list(REMOVE_ITEM TEST_OPS "test_dist_fuse_sgd_pass")
    list(REMOVE_ITEM TEST_OPS "test_dist_gradient_merge_pass")
    list(REMOVE_ITEM TEST_OPS "test_auto_parallel_amp_pass")
    list(REMOVE_ITEM TEST_OPS "test_auto_parallel_recompute_pass")
    list(REMOVE_ITEM TEST_OPS "test_auto_parallel_sharding_pass")
endif()

foreach(TEST_OP ${TEST_OPS})
    py_test_modules(${TEST_OP} MODULES ${TEST_OP})
    list(APPEND DIST_TEST_OPS ${TEST_OP})
    set_tests_properties(${TEST_OP} PROPERTIES TIMEOUT 120)
    set_tests_properties(${TEST_OP} PROPERTIES LABELS "RUN_TYPE=DIST")
endforeach(TEST_OP)
