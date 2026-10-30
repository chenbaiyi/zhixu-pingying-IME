cmake_minimum_required(VERSION 3.28)
project(ZhixuIME VERSION 0.1.0 LANGUAGES CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT MSVC)
  message(FATAL_ERROR "ZhixuIME requires Windows x64 with MSVC")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# This project has no C++ modules.  With CMake 4.2 + MSVC/Ninja, automatic
# module scanning can omit ordinary header dependencies from an incremental
# build, leaving consumers compiled against a stale class layout.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
add_compile_options(/utf-8 /W4 /permissive- /EHsc)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
find_package(Protobuf CONFIG REQUIRED)
find_package(tl-expected CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(minizip-ng CONFIG REQUIRED)
option(ZHIXU_ENABLE_LIBRIME "Build the verified development-only librime adapter" OFF)
if(ZHIXU_ENABLE_LIBRIME)
  include(cmake/verify_rime_assets.cmake)
  include(FetchContent)
  FetchContent_Declare(librime
    URL https://codeload.github.com/rime/librime/tar.gz/33e78140250125871856cdc5b42ddc6a5fcd3cd4
    URL_HASH SHA256=d3f48c2c58f718402229031d8d95fde9cac07ababa8fecf7d18b91946f27fee6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_STATIC ON CACHE BOOL "" FORCE)
  set(BUILD_TEST OFF CACHE BOOL "" FORCE)
  set(BUILD_DATA OFF CACHE BOOL "" FORCE)
  set(BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
  set(ENABLE_LOGGING OFF CACHE BOOL "" FORCE)
  # librime's bundled FindYamlCpp.cmake searches yaml-cpp.lib before
  # yaml-cppd.lib.  Pin the imported Debug artifact so its static CRT matches
  # this project when building the development-only Debug preset.
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    find_package(yaml-cpp CONFIG REQUIRED)
    get_target_property(zhixu_yaml_cpp_debug yaml-cpp::yaml-cpp IMPORTED_LOCATION_DEBUG)
    if(NOT zhixu_yaml_cpp_debug)
      message(FATAL_ERROR "yaml-cpp Debug artifact is required for librime")
    endif()
    set(YamlCpp_LIBRARY "${zhixu_yaml_cpp_debug}" CACHE FILEPATH "librime yaml-cpp library" FORCE)
  endif()
  FetchContent_GetProperties(librime)
  if(NOT librime_POPULATED)
    FetchContent_Populate(librime)
    execute_process(
      COMMAND ${CMAKE_COMMAND} -DLIBRIME_SOURCE_DIR=${librime_SOURCE_DIR}
              -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_librime_source_root.cmake
      COMMAND_ERROR_IS_FATAL ANY)
  endif()
  execute_process(
    COMMAND ${CMAKE_COMMAND} -DLIBRIME_SOURCE_DIR=${librime_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_librime_utf8.cmake
    COMMAND_ERROR_IS_FATAL ANY)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -DLIBRIME_SOURCE_DIR=${librime_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_librime_word_priority.cmake
    COMMAND_ERROR_IS_FATAL ANY)
  add_subdirectory(${librime_SOURCE_DIR} ${librime_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
add_library(zhixu_wire STATIC proto/ime.proto)
protobuf_generate(TARGET zhixu_wire LANGUAGE cpp)
target_include_directories(zhixu_wire PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(zhixu_wire PUBLIC protobuf::libprotobuf-lite)
add_library(zhixu_common STATIC
  src/common/ipc/named_pipe.cpp
  src/common/ipc/pipe_security.cpp
  src/common/protocol/envelope_codec.cpp
  src/common/protocol/protocol_handshake.cpp
  src/common/paths/runtime_paths.cpp
  src/common/settings/settings_store.cpp
  src/common/diagnostics/diagnostic_export.cpp
  src/common/diagnostics/sanitized_logger.cpp)
target_include_directories(zhixu_common PUBLIC src)
target_link_libraries(zhixu_common PUBLIC zhixu_wire tl::expected PRIVATE advapi32 shell32 ole32 nlohmann_json::nlohmann_json)
add_library(zhixu_input_host_core STATIC
  src/input_host/input_host_server.cpp
  src/input_host/engine/rime_engine.cpp
  src/input_host/prediction/prediction_dispatcher.cpp
  src/input_host/prediction/prediction_runtime.cpp
  src/input_host/prediction/prediction_service_client.cpp
  src/input_host/session/session_controller.cpp
  src/input_host/ui/candidate_presentation.cpp
  src/input_host/ui/candidate_window.cpp
  src/input_host/ui/candidate_window_host.cpp
  src/input_host/ui/recommendation_presentation.cpp
  src/input_host/ui/recommendation_window.cpp
  src/input_host/ui/recommendation_window_host.cpp
  src/input_host/ui/window_base.cpp
  src/input_host/ui/window_placement.cpp)
target_include_directories(zhixu_input_host_core PUBLIC src)
target_link_libraries(zhixu_input_host_core PUBLIC zhixu_common PRIVATE d2d1 dwrite comctl32 bcrypt)
if(ZHIXU_ENABLE_LIBRIME)
  target_include_directories(zhixu_input_host_core PRIVATE ${librime_SOURCE_DIR}/src)
  target_link_libraries(zhixu_input_host_core PRIVATE rime-static)
  target_compile_definitions(zhixu_input_host_core PRIVATE ZHIXU_ENABLE_LIBRIME=1)
  # Pinned BSD-3-Clause grammar component; statically linked for offline use.
  target_sources(zhixu_input_host_core PRIVATE
    third_party/octagram/src/gram_db.cc
    third_party/octagram/src/gram_encoding.cc
    third_party/octagram/src/grammar_module.cc
    third_party/octagram/src/octagram.cc)
  target_include_directories(zhixu_input_host_core PRIVATE
    ${librime_BINARY_DIR}/src ${librime_SOURCE_DIR}/include)
  target_compile_definitions(zhixu_input_host_core PRIVATE ZHIXU_ENABLE_OCTAGRAM=1)
endif()
add_library(zhixu_broker_core STATIC
  src/broker/settings_control.cpp
  src/broker/session_supervisor.cpp
  src/broker/win32_child_launcher.cpp)
target_include_directories(zhixu_broker_core PUBLIC src)
target_link_libraries(zhixu_broker_core PUBLIC zhixu_common)
add_library(zhixu_predict_core STATIC
  src/predict/imepack/routing_index.cpp
  src/predict/imepack/disk_index.cpp
  src/predict/imepack/index_builder.cpp
  src/predict/imepack/package_store.cpp
  src/predict/imepack/package_validator.cpp
  src/predict/personalization/personalization_store.cpp
  src/predict/prediction/prediction_coordinator.cpp
  src/predict/prediction/ranker.cpp
  src/predict/prediction/retriever.cpp)
target_include_directories(zhixu_predict_core PUBLIC src)
target_link_libraries(zhixu_predict_core PUBLIC zhixu_common PRIVATE
  MINIZIP::minizip-ng
  nlohmann_json::nlohmann_json
  bcrypt
  normaliz
  unofficial::sqlite3::sqlite3)
target_link_libraries(zhixu_broker_core PUBLIC zhixu_predict_core)
add_executable(InputHost src/input_host/main.cpp)
target_link_libraries(InputHost PRIVATE zhixu_input_host_core)
set(ZHIXU_INPUT_HOST_OUTPUT_NAME "InputHost" CACHE STRING "InputHost basename; use a separate name while a development host is running")
set_target_properties(InputHost PROPERTIES
  OUTPUT_NAME "${ZHIXU_INPUT_HOST_OUTPUT_NAME}"
  PDB_NAME "${ZHIXU_INPUT_HOST_OUTPUT_NAME}")
if(ZHIXU_ENABLE_LIBRIME)
  # Run even when only a data file changed and the executable needs no relink.
  add_custom_target(zhixu_rime_data ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_CURRENT_SOURCE_DIR}/assets/rime/full_pinyin
      $<TARGET_FILE_DIR:InputHost>/rime)
  add_dependencies(InputHost zhixu_rime_data)
endif()
add_executable(ZhixuImeBroker src/broker/main.cpp)
target_link_libraries(ZhixuImeBroker PRIVATE zhixu_broker_core)
add_executable(CorpusTool src/predict_service/corpus_tool.cpp)
target_link_libraries(CorpusTool PRIVATE zhixu_predict_core psapi)
if(ZHIXU_ENABLE_LIBRIME)
  find_package(RapidJSON CONFIG REQUIRED)
  find_package(OpenCC CONFIG REQUIRED)
  add_executable(CorpusNormalize src/predict_service/corpus_normalize.cpp)
  target_link_libraries(CorpusNormalize PRIVATE OpenCC::OpenCC nlohmann_json::nlohmann_json)
  target_compile_definitions(CorpusNormalize PRIVATE Opencc_BUILT_AS_STATIC)
endif()
add_executable(PredictService src/predict_service/main.cpp)
target_link_libraries(PredictService PRIVATE zhixu_predict_core)
set(ZHIXU_PREDICT_OUTPUT_NAME "PredictService" CACHE STRING "PredictService basename")
set(ZHIXU_BROKER_OUTPUT_NAME "ZhixuImeBroker" CACHE STRING "Broker basename")
set_target_properties(PredictService PROPERTIES OUTPUT_NAME "${ZHIXU_PREDICT_OUTPUT_NAME}")
set_target_properties(ZhixuImeBroker PROPERTIES OUTPUT_NAME "${ZHIXU_BROKER_OUTPUT_NAME}")
add_library(zhixu_tsf_core STATIC
  src/tsf/edit_session.cpp
  src/tsf/input_host_transport.cpp
  src/tsf/input_scope_policy.cpp
  src/tsf/key_event_sink.cpp
  src/tsf/module_lifetime.cpp
  src/tsf/preedit_state.cpp
  src/tsf/text_service.cpp
  src/tsf/tsf_text_service.cpp)
target_include_directories(zhixu_tsf_core PUBLIC src)
target_link_libraries(zhixu_tsf_core PUBLIC zhixu_common PRIVATE bcrypt)
add_library(ImeTsf SHARED
  src/tsf/class_factory.cpp
  src/tsf/dllmain.cpp)
target_link_libraries(ImeTsf PRIVATE zhixu_tsf_core advapi32 ole32 uuid)
set(ZHIXU_TSF_OUTPUT_NAME "ImeTsf" CACHE STRING "TSF DLL basename; use a distinct name for development while the previous DLL is loaded")
set_target_properties(ImeTsf PROPERTIES
  OUTPUT_NAME "${ZHIXU_TSF_OUTPUT_NAME}"
  PDB_NAME "${ZHIXU_TSF_OUTPUT_NAME}")
target_link_options(ImeTsf PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/tsf/ime_tsf.def")
option(BUILD_TESTING "Build ZhixuIME native tests" ON)
if(BUILD_TESTING)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)
  add_executable(zhixu_runtime_recovery_test tests/input_host/runtime_recovery_test.cpp)
  target_link_libraries(zhixu_runtime_recovery_test PRIVATE zhixu_input_host_core zhixu_tsf_core)
  add_test(NAME RuntimeRecovery COMMAND zhixu_runtime_recovery_test)
  add_executable(zhixu_common_tests
    tests/common/envelope_codec_test.cpp
    tests/common/protocol_handshake_test.cpp
    tests/common/sanitized_logger_test.cpp
    tests/common/named_pipe_test.cpp
    tests/common/runtime_paths_test.cpp
    tests/common/settings_store_test.cpp
    tests/common/diagnostic_export_test.cpp)
  target_link_libraries(zhixu_common_tests PRIVATE zhixu_common GTest::gtest_main)
  add_executable(zhixu_input_host_tests
    tests/input_host/session_controller_test.cpp
    tests/input_host/prediction_dispatcher_test.cpp
    tests/input_host/input_host_server_test.cpp
    tests/input_host/window_placement_test.cpp)
  target_link_libraries(zhixu_input_host_tests PRIVATE zhixu_input_host_core zhixu_predict_core zhixu_tsf_core GTest::gtest_main)
  target_compile_definitions(zhixu_input_host_tests PRIVATE ZHIXU_TEST_PREDICT_SERVICE_EXE="$<TARGET_FILE:PredictService>")
  add_dependencies(zhixu_input_host_tests PredictService)
  if(ZHIXU_ENABLE_LIBRIME)
    target_sources(zhixu_input_host_tests PRIVATE tests/input_host/rime_engine_test.cpp)
    target_compile_definitions(zhixu_input_host_tests PRIVATE
      ZHIXU_RIME_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/assets/rime/minimal"
      ZHIXU_RIME_FULL_DATA="$<TARGET_FILE_DIR:InputHost>/rime")
    target_link_libraries(zhixu_input_host_tests PRIVATE zhixu_tsf_core)
    add_dependencies(zhixu_input_host_tests zhixu_rime_data)
    add_test(NAME RimeEngine COMMAND zhixu_input_host_tests --gtest_filter=RimeEngine.*)
    add_test(NAME FullPinyin COMMAND zhixu_input_host_tests --gtest_filter=FullPinyin.*)
    set_tests_properties(FullPinyin PROPERTIES TIMEOUT 900)
  endif()
  add_executable(zhixu_broker_tests
    tests/broker/session_supervisor_test.cpp
    tests/broker/settings_control_test.cpp)
  target_link_libraries(zhixu_broker_tests PRIVATE zhixu_broker_core GTest::gtest_main MINIZIP::minizip-ng)
  add_executable(zhixu_tsf_tests
    tests/tsf/input_scope_policy_test.cpp
    tests/tsf/input_host_transport_test.cpp
    tests/tsf/key_event_sink_test.cpp
    tests/tsf/preedit_state_test.cpp
    tests/tsf/text_service_test.cpp
    tests/tsf/tsf_text_service_test.cpp
    tests/tsf/tsf_com_exports_test.cpp)
  target_link_libraries(zhixu_tsf_tests PRIVATE zhixu_tsf_core GTest::gtest_main)
  target_compile_definitions(zhixu_tsf_tests PRIVATE
    ZHIXU_TSF_TEST_DLL="$<TARGET_FILE:ImeTsf>")
  add_executable(zhixu_predict_tests
    tests/predict/package_store_test.cpp
    tests/predict/package_validator_test.cpp
    tests/predict/personalization_store_test.cpp
    tests/predict/prediction_coordinator_test.cpp
    tests/predict/ranker_test.cpp
    tests/predict/retriever_test.cpp)
  target_link_libraries(zhixu_predict_tests PRIVATE
    zhixu_predict_core
    GTest::gtest_main
    MINIZIP::minizip-ng)
  add_dependencies(zhixu_tsf_tests ImeTsf)
  # GoogleTest discovery writes a JSON listing beside the executable. The
  # current GTest package cannot open that path when the worktree has CJK
  # characters, so register the stable suites directly instead.
  foreach(suite EnvelopeCodec ProtocolHandshake SanitizedLogger RuntimePaths NamedPipe SettingsStore DiagnosticExport)
    add_test(NAME ${suite} COMMAND zhixu_common_tests --gtest_filter=${suite}.*)
  endforeach()
  add_test(NAME RuntimeSettings COMMAND zhixu_input_host_tests --gtest_filter=RuntimeSettings.*:RuntimeRecovery.*)
  add_test(NAME ApplicationSettingsPolicy COMMAND zhixu_tsf_tests --gtest_filter=RuntimeSettings.*)
  add_test(NAME SessionController COMMAND zhixu_input_host_tests --gtest_filter=SessionController.*)
  add_test(NAME InputHostPipe COMMAND zhixu_input_host_tests --gtest_filter=InputHostPipe.*:InputHostServer.*)
  add_executable(zhixu_window_smoke tests/input_host/window_smoke.cpp)
  target_link_libraries(zhixu_window_smoke PRIVATE zhixu_input_host_core dwmapi)
  add_test(NAME WindowSmoke COMMAND zhixu_window_smoke)
  add_test(NAME WindowPlacement COMMAND zhixu_input_host_tests --gtest_filter=WindowPlacement.*)
  add_test(NAME CandidateWindow COMMAND zhixu_input_host_tests --gtest_filter=CandidateWindow.*)
  add_test(NAME CandidatePresentation COMMAND zhixu_input_host_tests --gtest_filter=CandidatePresentation.*)
  add_test(NAME RecommendationPresentation COMMAND zhixu_input_host_tests --gtest_filter=RecommendationPresentation.*)
  add_test(NAME PredictionDispatcher COMMAND zhixu_input_host_tests --gtest_filter=PredictionDispatcher.*)
  add_test(NAME PredictionRuntime COMMAND zhixu_input_host_tests --gtest_filter=PredictionRuntime.*)
  add_test(NAME PredictionServiceClient COMMAND zhixu_input_host_tests --gtest_filter=PredictionServiceClient.*)
  add_test(NAME RecommendationWindow COMMAND zhixu_input_host_tests --gtest_filter=RecommendationWindow.*)
  add_test(NAME SessionSupervisor COMMAND zhixu_broker_tests --gtest_filter=SessionSupervisor.*)
  add_test(NAME SettingsControl COMMAND zhixu_broker_tests --gtest_filter=SettingsControl.*)
  add_test(NAME PackageStore COMMAND zhixu_predict_tests --gtest_filter=PackageStore.*)
  add_test(NAME PackageValidator COMMAND zhixu_predict_tests --gtest_filter=PackageValidator.*)
  add_test(NAME Ranker COMMAND zhixu_predict_tests --gtest_filter=Ranker.*)
  add_test(NAME RoutingIndex COMMAND zhixu_predict_tests --gtest_filter=RoutingIndex.*)
  add_test(NAME Retriever COMMAND zhixu_predict_tests --gtest_filter=Retriever.*)
  add_test(NAME Personalization COMMAND zhixu_predict_tests --gtest_filter=PersonalizationStore.*)
  add_test(NAME PredictionCoordinator COMMAND zhixu_predict_tests --gtest_filter=PredictionCoordinator.*)
  add_test(NAME InputScope COMMAND zhixu_tsf_tests --gtest_filter=InputScopePolicy.*)
  add_test(NAME InputHostTransport COMMAND zhixu_tsf_tests --gtest_filter=InputHostTransport.*)
  add_test(NAME KeyEventSink COMMAND zhixu_tsf_tests --gtest_filter=KeyEventSink.*)
  add_test(NAME PreeditState COMMAND zhixu_tsf_tests --gtest_filter=PreeditState.*)
  add_test(NAME TextService COMMAND zhixu_tsf_tests --gtest_filter=TextService.*)
  add_test(NAME CommitEditSession COMMAND zhixu_tsf_tests --gtest_filter=CommitEditSession.*)
  add_test(NAME TsfTextService COMMAND zhixu_tsf_tests --gtest_filter=TsfTextService.*)
  add_test(NAME TsfComExports COMMAND zhixu_tsf_tests --gtest_filter=TsfDll.*)
endif()

# MSVC's localized /showIncludes output is not recorded by Ninja on this
# toolchain (see the empty `ninja -t deps` result).  Make project headers
# explicit object dependencies so an incremental build cannot mix objects
# compiled against different class layouts.
file(GLOB_RECURSE zhixu_project_headers CONFIGURE_DEPENDS LIST_DIRECTORIES false
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
set(zhixu_header_dependency_targets
  zhixu_common
  zhixu_input_host_core
  zhixu_broker_core
  zhixu_predict_core
  zhixu_tsf_core
  ImeTsf
  InputHost
  ZhixuImeBroker
  PredictService
  CorpusTool)
if(BUILD_TESTING)
  list(APPEND zhixu_header_dependency_targets
    zhixu_common_tests
    zhixu_window_smoke
    zhixu_input_host_tests
    zhixu_broker_tests
    zhixu_tsf_tests
    zhixu_predict_tests)
endif()
foreach(zhixu_target IN LISTS zhixu_header_dependency_targets)
  get_target_property(zhixu_target_sources ${zhixu_target} SOURCES)
  foreach(zhixu_source IN LISTS zhixu_target_sources)
    if(zhixu_source MATCHES "\\.(cc|cpp|cxx)$")
      set_property(SOURCE ${zhixu_source} APPEND PROPERTY OBJECT_DEPENDS "${zhixu_project_headers}")
    endif()
  endforeach()
endforeach()
