PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=ai
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# The generic `test/*` filter also selects DuckDB's built-in tests because the
# unittest runner reports those with the same prefix. Keep this extension's test
# target scoped to its SQLLogic suite.
TESTS_BASE_DIRECTORY = "test/sql/"
