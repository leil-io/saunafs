#pragma once

#include <boost/config.hpp>
#include <boost/dll/alias.hpp>
#include <boost/dll/import.hpp>

/// IPlugin is the base interface for all plugins.
/// It contains the basic information for all plugins.
class BOOST_SYMBOL_VISIBLE IPlugin {
public:
	/// Default constructor
	IPlugin() = default;
	/// Virtual destructor for correct polymorphism
	virtual ~IPlugin() = default;

	/// Should be called after instantiation to initialize the plugin.
	/// This way, the constructor can be simple and marked as noexcept if
	/// needed.
	virtual bool initialize() = 0;

	/// Returns the plugin name
	virtual std::string name() = 0;
	/// Returns the plugin version
	virtual unsigned int version() = 0;
	/// String representation of this plugin
	virtual std::string toString() = 0;
};
