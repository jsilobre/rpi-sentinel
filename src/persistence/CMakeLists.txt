add_library(persistence STATIC
    HistoryStore.cpp
    SqliteHistoryHandler.cpp
)

target_include_directories(persistence
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${SQLITE3_INCLUDE_DIRS}
)

target_link_libraries(persistence
    PUBLIC  alerts events
    PRIVATE ${SQLITE3_LIBRARIES}
)

target_link_directories(persistence PUBLIC ${SQLITE3_LIBRARY_DIRS})

apply_compiler_options(persistence)
