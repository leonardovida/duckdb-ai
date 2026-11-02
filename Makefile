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

# The shared tidy target's src/.*/ filter skips C++ files directly inside src/.
# Keep its configure step, then analyze this extension's actual source files.
# libcurl requires long ABI types; keep them and explicit printf diagnostic casts.
.PHONY: tidy-check-ai
tidy-check-ai: tidy-check
	cd build/tidy && python3 ../../duckdb/scripts/run-clang-tidy.py '$(PROJ_DIR)src/' -header-filter '$(PROJ_DIR)src/' -quiet -checks=-google-runtime-int ${TIDY_THREAD_PARAMETER} ${TIDY_BINARY_PARAMETER} ${TIDY_PERFORM_CHECKS}

# Deterministic smoke suites against local mock providers; CI runs this target after `make release`.
# mock_provider_smoke.py also runs the provider, Jev, schema and transport smokes it imports.
SMOKE_TESTS = mock_provider docs_coverage decision_providers chaining native_types output_parsing extension_upload \
	resumable_enrichment jev_batch_evaluation accuracy_gate execution_deadline
SHARED_LIBRARY_SUFFIX := $(if $(filter Darwin,$(shell uname -s)),dylib,so)

.PHONY: smoke
smoke:
	@set -e; for test in $(SMOKE_TESTS); do echo "== $${test}_smoke.py"; python3 test/smoke/$${test}_smoke.py; done
	python3 test/smoke/reliability_smoke.py --library build/release/src/libduckdb.$(SHARED_LIBRARY_SUFFIX)
