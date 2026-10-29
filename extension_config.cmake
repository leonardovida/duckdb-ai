# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(ai
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    EXTENSION_VERSION 0.9.0
)

# Since DuckDB 2.0 an extension is only built into the shell and unittest binaries
# when listed here. The smoke tests run build/release/duckdb with ai built in.
duckdb_extension_statically_link(ai)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)
