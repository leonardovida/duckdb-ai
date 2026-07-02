#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! DuckDB extension entry point for registering the ai extension SQL surface.
class AiExtension : public Extension {
public:
	//! Register settings, secrets, scalar functions, aggregate functions, and table functions.
	void Load(ExtensionLoader &db) override;
	//! Return the extension catalog name.
	std::string Name() override;
	//! Return the build/version string exposed by DuckDB.
	std::string Version() const override;
};

} // namespace duckdb
