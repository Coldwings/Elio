if(NOT DEFINED ELIO_SOURCE_DIR)
    message(FATAL_ERROR "ELIO_SOURCE_DIR is required")
endif()

file(READ "${ELIO_SOURCE_DIR}/examples/CMakeLists.txt" _examples_cmake)
file(READ "${ELIO_SOURCE_DIR}/examples/rdma_gpu_bw.cpp" _rdma_gpu_bw)

string(REGEX MATCH
    "if\\([ \t\r\n]*TARGET[ \t\r\n]+elio_rdma_cuda[ \t\r\n]+AND[ \t\r\n]+TARGET[ \t\r\n]+elio_rdma_cm[ \t\r\n]*\\)[ \t\r\n]+add_executable\\([ \t\r\n]*rdma_gpu_bw"
    _rdma_gpu_bw_gate "${_examples_cmake}")
if(NOT _rdma_gpu_bw_gate)
    message(FATAL_ERROR
        "rdma_gpu_bw must be gated on both elio_rdma_cuda and elio_rdma_cm")
endif()

foreach(_stale_api IN ITEMS
    "endpoint::accept"
    "endpoint::connect"
    "post_recv"
    "sync_wait"
)
    string(FIND "${_rdma_gpu_bw}" "${_stale_api}" _stale_pos)
    if(NOT _stale_pos EQUAL -1)
        message(FATAL_ERROR "rdma_gpu_bw still references stale API: ${_stale_api}")
    endif()
endforeach()

foreach(_required_api IN ITEMS
    "rdma_cm::event_channel"
    "rdma_ibverbs::acceptor"
    "rdma_ibverbs::connect"
    "ep.start_cq_pump"
    "ep.conn().recv"
    "gai_strerror"
    "cfg.buf_mode != \"gpu\""
)
    string(FIND "${_rdma_gpu_bw}" "${_required_api}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "rdma_gpu_bw is missing current API usage: ${_required_api}")
    endif()
endforeach()
